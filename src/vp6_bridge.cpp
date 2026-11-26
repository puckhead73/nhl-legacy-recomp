// VP6 host-decode bridge (env NHL_VP6_BRIDGE=1).
//
// The recompiled VP6 decoder has an unfixed arithmetic bug in its VMX128
// float IDCT (docs/vp6-fork-investigation.md): high-AC blocks decode with
// columns crushed to zero (green/black striping). Rather than patch the
// generated math, this bridge replaces the DECODED PIXELS at the movie
// player's publish seam with frames decoded on the host by ffmpeg:
//
//   input : the SDK's NtCreateFile exports the last opened *.vp6 guest path
//           in the NHL_VP6_LAST_OPEN process env var; the movie-setup hook
//           (sub_8277ABB8) maps it to a host file under --game_data_root.
//   decode: ffmpeg (ffmpeg.exe on Windows; PATH or NHL_VP6_FFMPEG) is spawned per movie with
//           "-f rawvideo -pix_fmt yuv420p pipe:1"; a reader thread buffers
//           the raw frames (bounded).
//   output: the plane-publish hook (sub_8277CC98, see diag_hooks.cpp) calls
//           Vp6BridgePublish once per published frame; we overwrite the ring
//           slot's Y/U/V planes with the next ffmpeg frame. Rows are written
//           bottom-up (the ring renders vertically flipped - verified with a
//           gradient test pattern). On stream EOF the process is respawned
//           from frame 0 so looping menu movies keep playing.
//
// Audio is untouched (the guest EA-audio path has always been correct); sync
// is inherently guest-paced because we only swap pixels at publish time.

#include "vp6_bridge.h"

#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>

#include "platform/host_compat.h"  // _strnicmp, GetTickCount, env helpers

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

bool BridgeOn() {
  // Default ON: the recompiled decoder's IDCT bug corrupts every bright
  // movie frame, and the bridge degrades gracefully (guest frames) when
  // ffmpeg or the loose movie files are absent. NHL_VP6_BRIDGE=0 opts out.
  static const bool on = [] {
    const char* e = std::getenv("NHL_VP6_BRIDGE");
    return !e || !*e || *e != '0';
  }();
  return on;
}

void BridgeLog(const char* fmt, ...) {
  FILE* f = std::fopen("vp6_bridge.txt", "a");
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fclose(f);
}

// ---- movie decode state ------------------------------------------------------

struct BridgeState {
  std::mutex m;
  std::condition_variable cv;
  std::string host_path;   // current movie file on host
#if defined(_WIN32)
  HANDLE proc = nullptr;   // ffmpeg child
  HANDLE pipe_rd = nullptr;
#else
  pid_t proc = 0;          // ffmpeg child
  int pipe_rd = -1;
#endif
  std::thread reader;
  // Frame-granular FIFO: whole decoded frames as moved vectors. NEVER buffer
  // the raw byte stream in a std::deque<uint8_t> - MSVC's deque uses 16-byte
  // blocks for byte elements, so bulk inserts/erases become millions of tiny
  // allocations and the publish (game) thread stalls for seconds per frame.
  std::deque<std::vector<uint8_t>> frames;
  std::vector<uint8_t> staging;         // partial-frame accumulation
  std::atomic<size_t> frame_size{0};    // w*h*3/2, set by the publish side
  std::atomic<bool> eof{false};
  std::atomic<bool> reader_stop{false};
  uint64_t frames_served = 0;
};

BridgeState& S() {
  static BridgeState s;
  return s;
}

constexpr size_t kMaxFrames = 24;             // decoded frames queued ahead
constexpr size_t kMaxStaging = 32ull << 20;   // pre-first-publish cap

#if defined(_WIN32)
using PipeHandle = HANDLE;
// Blocking read; false on EOF/error.
bool ReadPipe(PipeHandle h, uint8_t* buf, size_t cap, size_t* got) {
  DWORD n = 0;
  if (!ReadFile(h, buf, DWORD(cap), &n, nullptr) || n == 0) return false;
  *got = n;
  return true;
}
#else
using PipeHandle = int;
bool ReadPipe(PipeHandle fd, uint8_t* buf, size_t cap, size_t* got) {
  for (;;) {
    const ssize_t n = ::read(fd, buf, cap);
    if (n > 0) {
      *got = size_t(n);
      return true;
    }
    if (n < 0 && errno == EINTR) continue;
    return false;  // EOF, or the fd was shut down by StopMovie
  }
}
#endif

void ReaderThread(PipeHandle pipe_rd) {
  std::vector<uint8_t> chunk(1 << 20);
  for (;;) {
    if (S().reader_stop.load()) break;
    {
      // Backpressure: wait until the publish side drains the FIFO.
      std::unique_lock<std::mutex> lk(S().m);
      S().cv.wait_for(lk, std::chrono::milliseconds(100), [] {
        return S().reader_stop.load() || S().frames.size() < kMaxFrames ||
               (S().frame_size.load() == 0 &&
                S().staging.size() < kMaxStaging);
      });
      if (S().reader_stop.load()) break;
      if (S().frames.size() >= kMaxFrames) continue;
      if (S().frame_size.load() == 0 && S().staging.size() >= kMaxStaging)
        continue;
    }
    size_t got = 0;
    if (!ReadPipe(pipe_rd, chunk.data(), chunk.size(), &got)) {
      S().eof.store(true);
      std::lock_guard<std::mutex> lk(S().m);
      S().cv.notify_all();
      break;
    }
    std::lock_guard<std::mutex> lk(S().m);
    BridgeState& s = S();
    s.staging.insert(s.staging.end(), chunk.data(), chunk.data() + got);
    size_t fs = s.frame_size.load();
    if (fs) {
      size_t off = 0;
      while (s.staging.size() - off >= fs) {
        s.frames.emplace_back(s.staging.begin() + off,
                              s.staging.begin() + off + fs);
        off += fs;
      }
      if (off) s.staging.erase(s.staging.begin(), s.staging.begin() + off);
    }
    s.cv.notify_all();
  }
}

void StopMovie() {
  BridgeState& s = S();
  s.reader_stop.store(true);
  s.cv.notify_all();
#if defined(_WIN32)
  if (s.pipe_rd) CancelIoEx(s.pipe_rd, nullptr);
  if (s.reader.joinable()) s.reader.join();
  if (s.pipe_rd) CloseHandle(s.pipe_rd);
  if (s.proc) {
    TerminateProcess(s.proc, 0);
    CloseHandle(s.proc);
  }
  s.pipe_rd = nullptr;
  s.proc = nullptr;
#else
  // Kill the child first: its stdout closes, so a reader blocked in read()
  // returns 0 and the thread exits.
  if (s.proc > 0) ::kill(s.proc, SIGKILL);
  if (s.reader.joinable()) s.reader.join();
  if (s.pipe_rd >= 0) ::close(s.pipe_rd);
  if (s.proc > 0) ::waitpid(s.proc, nullptr, 0);
  s.pipe_rd = -1;
  s.proc = 0;
#endif
  s.reader_stop.store(false);
  s.eof.store(false);
  std::lock_guard<std::mutex> lk(s.m);
  s.frames.clear();
  s.staging.clear();
}

std::string FfmpegPath() {
  if (const char* e = std::getenv("NHL_VP6_FFMPEG"); e && *e) return e;
#if defined(_WIN32)
  return "ffmpeg.exe";  // PATH
#else
  return "ffmpeg";  // PATH (posix_spawnp)
#endif
}

#if defined(_WIN32)
bool StartMovie(const std::string& host_path) {
  BridgeState& s = S();
  StopMovie();
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 8 << 20)) return false;
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  std::string cmd = "\"" + FfmpegPath() +
                    "\" -v error -i \"" + host_path +
                    "\" -map 0:v:0 -f rawvideo -pix_fmt yuv420p pipe:1";
  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION pi{};
  std::vector<char> cmdbuf(cmd.begin(), cmd.end());
  cmdbuf.push_back(0);
  BOOL ok = CreateProcessA(nullptr, cmdbuf.data(), nullptr, nullptr, TRUE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) {
    CloseHandle(rd);
    BridgeLog("spawn FAILED err=%lu cmd=%s", GetLastError(), cmd.c_str());
    // Remember the path so the publish hook doesn't retry the spawn per
    // frame (e.g. ffmpeg not installed) - the guest's own frames show.
    s.host_path = host_path;
    return false;
  }
  CloseHandle(pi.hThread);
  s.proc = pi.hProcess;
  s.pipe_rd = rd;
  s.host_path = host_path;
  s.frames_served = 0;
  s.reader = std::thread(ReaderThread, rd);
  BridgeLog("spawned ffmpeg for %s", host_path.c_str());
  return true;
}
#else
bool StartMovie(const std::string& host_path) {
  BridgeState& s = S();
  StopMovie();
  int fds[2];
  if (::pipe2(fds, O_CLOEXEC) != 0) return false;
  // Same bounded-pipe intent as the 8 MB CreatePipe buffer (best effort).
  ::fcntl(fds[0], F_SETPIPE_SZ, 8 << 20);
  const std::string ffmpeg = FfmpegPath();
  std::vector<std::string> args = {ffmpeg, "-v", "error", "-i", host_path,
                                   "-map", "0:v:0", "-f", "rawvideo",
                                   "-pix_fmt", "yuv420p", "pipe:1"};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);  // dup2 clears CLOEXEC
  pid_t pid = 0;
  const int err = ::posix_spawnp(&pid, ffmpeg.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  ::close(fds[1]);
  if (err != 0) {
    ::close(fds[0]);
    BridgeLog("spawn FAILED err=%d (%s) ffmpeg=%s", err, std::strerror(err), ffmpeg.c_str());
    s.host_path = host_path;  // don't retry the spawn per frame
    return false;
  }
  s.proc = pid;
  s.pipe_rd = fds[0];
  s.host_path = host_path;
  s.frames_served = 0;
  s.reader = std::thread(ReaderThread, fds[0]);
  BridgeLog("spawned ffmpeg for %s", host_path.c_str());
  return true;
}
#endif

// Map a guest VFS path (game:\..., cache:\..., update:\...) to a host file
// under --game_data_root (parsed from the command line).
std::string GameDataRoot() {
  static const std::string root = [] {
#if defined(_WIN32)
    std::string cl = GetCommandLineA();
#else
    // argv joined with spaces, quoting args that contain one, so the parser
    // below sees the same shape as GetCommandLineA.
    std::string cl;
    if (std::FILE* f = std::fopen("/proc/self/cmdline", "rb")) {
      std::string arg;
      int c;
      auto flush = [&] {
        if (!cl.empty()) cl += ' ';
        cl += arg.find(' ') != std::string::npos ? "\"" + arg + "\"" : arg;
        arg.clear();
      };
      while ((c = std::fgetc(f)) != EOF) {
        if (c == 0) flush(); else arg += char(c);
      }
      if (!arg.empty()) flush();
      std::fclose(f);
    }
#endif
    const char* key = "--game_data_root";
    size_t p = cl.find(key);
    if (p == std::string::npos) return std::string();
    p += std::strlen(key);
    while (p < cl.size() && (cl[p] == ' ' || cl[p] == '=')) ++p;
    std::string v;
    if (p < cl.size() && cl[p] == '"') {
      size_t q = cl.find('"', p + 1);
      v = cl.substr(p + 1, q - p - 1);
    } else {
      size_t q = cl.find(' ', p);
      v = cl.substr(p, q - p);
    }
    return v;
  }();
  return root;
}

std::string MapGuestPath(std::string g) {
  for (auto& c : g)
    if (c == '/') c = '\\';
#if defined(_WIN32)
  const std::string sep = "\\";
#else
  const std::string sep = "/";
#endif
  auto strip = [&](const char* pfx) -> bool {
    size_t n = std::strlen(pfx);
    if (g.size() > n && _strnicmp(g.c_str(), pfx, n) == 0) {
      g = g.substr(n);
      return true;
    }
    return false;
  };
  std::string root = GameDataRoot();
  if (root.empty()) return {};
  auto host = [&](std::string rel) {
#if !defined(_WIN32)
    for (auto& c : rel)
      if (c == '\\') c = '/';
#endif
    return root + sep + rel;
  };
  if (strip("game:\\")) return host(g);
  // cache:\X overlays game_data_root\_compiled\X (loose-asset tree).
  if (strip("cache:\\")) return host("_compiled\\" + g);
  if (strip("update:\\")) return host(g);
  // Raw device path - try as-is under the root.
  return host(g);
}

}  // namespace

// Start (or switch) the decoder to whatever .vp6 the guest opened last. The
// file open happens asynchronously AFTER the movie-setup call, so this runs
// lazily from the publish hook too.
static void MaybeStartFromEnv(const char* who) {
  std::string host;
  // Process env, not the CRT copy: the producers (loose_tree_device.cpp, and the
  // SDK's NtCreateFile when built with sdk/rexglue-vp6-sharedmem-fixes.patch)
  // publish via SetEnvironmentVariableA (setenv off Windows).
  std::string hostbuf = nhl_host::GetProcessEnv("NHL_VP6_LAST_OPEN_HOST");
  if (!hostbuf.empty()) {
    host = hostbuf;  // loose-tree open: already a host path
  } else {
    std::string guest = nhl_host::GetProcessEnv("NHL_VP6_LAST_OPEN");
    if (guest.empty()) return;
    host = MapGuestPath(guest);
  }
  // Already decoding it - or already tried and failed (don't respawn per
  // frame; StartMovie records the path on spawn failure).
  if (host == S().host_path) return;
  std::error_code ec;
  const bool exists = std::filesystem::exists(host, ec);
  BridgeLog("%s: host='%s' exists=%d", who, host.c_str(), int(exists));
  if (!exists) return;
  StartMovie(host);
}

void Vp6BridgeOnMovieSetup() {
  if (!BridgeOn()) return;
  MaybeStartFromEnv("setup");
}

void Vp6BridgePublish(uint8_t* y, uint8_t* u, uint8_t* v, uint32_t w,
                      uint32_t h, uint32_t y_pitch, uint32_t c_pitch) {
  if (!BridgeOn()) return;
  MaybeStartFromEnv("publish");
  BridgeState& s = S();
  if (!s.proc) return;
  const size_t ysz = size_t(w) * h;
  const size_t csz = size_t(w / 2) * (h / 2);
  const size_t fsz = ysz + 2 * csz;
  std::vector<uint8_t> frame;
  {
    std::lock_guard<std::mutex> lk(s.m);
    if (s.frame_size.load() != fsz) {
      // First publish (or dimension change): teach the reader the frame size
      // and slice whatever it staged so far.
      s.frame_size.store(fsz);
      size_t off = 0;
      while (s.staging.size() - off >= fsz) {
        s.frames.emplace_back(s.staging.begin() + off,
                              s.staging.begin() + off + fsz);
        off += fsz;
      }
      if (off) s.staging.erase(s.staging.begin(), s.staging.begin() + off);
    }
    if (!s.frames.empty()) {
      frame = std::move(s.frames.front());
      s.frames.pop_front();
      s.cv.notify_all();
    }
  }
  if (frame.empty()) {
    if (s.eof.load()) {
      // Movie looped (menu attract) - restart the decode from frame 0.
      std::string path = s.host_path;
      uint64_t served = s.frames_served;
      BridgeLog("eof after %llu frames - respawning %s",
                (unsigned long long)served, path.c_str());
      StartMovie(path);
    }
    return;  // underrun: keep the guest's own frame this once
  }
  // The ring renders vertically flipped relative to write order: write frame
  // row r into plane row (h-1-r).
  const uint8_t* fy = frame.data();
  const uint8_t* fu = frame.data() + ysz;
  const uint8_t* fv = frame.data() + ysz + csz;
  if (y) {
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(y + size_t(h - 1 - r) * y_pitch, fy + size_t(r) * w, w);
  }
  const uint32_t cw = w / 2, ch = h / 2;
  if (u) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(u + size_t(ch - 1 - r) * c_pitch, fu + size_t(r) * cw, cw);
  }
  if (v) {
    for (uint32_t r = 0; r < ch; ++r)
      std::memcpy(v + size_t(ch - 1 - r) * c_pitch, fv + size_t(r) * cw, cw);
  }
  ++s.frames_served;
  if (s.frames_served % 120 == 0) {
    BridgeLog("served %llu frames tick=%lu queue=%zu",
              (unsigned long long)s.frames_served,
              (unsigned long)GetTickCount(), s.frames.size());
  }
}
