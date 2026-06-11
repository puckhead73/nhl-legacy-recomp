#include "manifest.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string_view>

namespace packager {

namespace {

std::string Trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

// Strips surrounding quotes from a TOML basic string; passes bare values
// (integers) through unchanged. Comments must be removed by the caller.
std::string Unquote(const std::string& v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
    return v.substr(1, v.size() - 2);
  }
  return v;
}

}  // namespace

bool LoadPayloadManifest(const std::filesystem::path& path,
                         PayloadManifest& out, Error& err) {
  std::ifstream in(path);
  if (!in) {
    err = {ErrorCode::kBadPayload, "cannot open " + path.string()};
    return false;
  }

  std::string section;
  std::string line;
  while (std::getline(in, line)) {
    // Strip comments (the manifest we generate never puts '#' in strings).
    if (auto hash = line.find('#'); hash != std::string::npos) {
      line.erase(hash);
    }
    std::string t = Trim(line);
    if (t.empty()) continue;
    if (t.front() == '[' && t.back() == ']') {
      section = ToLower(Trim(std::string_view(t).substr(1, t.size() - 2)));
      continue;
    }
    auto eq = t.find('=');
    if (eq == std::string::npos) continue;
    std::string key = ToLower(Trim(std::string_view(t).substr(0, eq)));
    std::string value = Unquote(Trim(std::string_view(t).substr(eq + 1)));

    if (section == "tool") {
      if (key == "version") out.tool_version = value;
      else if (key == "sdk_version") out.sdk_version = value;
      else if (key == "port_build") out.port_build = value;
    } else if (section == "xex") {
      if (key == "title") out.xex_title = value;
      else if (key == "sha256") out.xex_sha256 = ToLower(value);
      else if (key == "size_bytes") out.xex_size_bytes = std::stoull(value);
    } else if (section == "port") {
      if (key == "exe") out.port_exe = value;
    }
  }

  if (out.xex_sha256.size() != 64 || out.port_exe.empty()) {
    err = {ErrorCode::kBadPayload,
           "manifest is missing xex.sha256 or port.exe: " + path.string()};
    return false;
  }
  return true;
}

}  // namespace packager
