//! vp6conv — convert any video into an EA-container VP6 (.vp6) movie for
//! NHL Legacy (and other EA Sports titles that use the same `MVhd` container).
//!
//! Pipeline:
//!   1. ffmpeg          — decode/scale/refps the source to a raw Y4M (yuv420p,
//!                        dimensions forced to a multiple of 16, which VP6 requires).
//!   2. nihav-encoder   — encode the Y4M to a VP6 elementary stream wrapped in the
//!                        EA container (`MVhd` + `MV0K`/`MV0F` frames). Video only —
//!                        NihAV's EA muxer does not write audio.
//!   3. (optional) splice — graft the audio chunks (`SCHl`/`SCCl`/`SCDl`/`SCEl`)
//!                        from a reference .vp6 into the freshly-encoded video,
//!                        byte-for-byte. The Xbox 360 EA audio codec cannot be
//!                        re-encoded by any open tool, so we never touch the audio
//!                        bytes — we reuse the original movie's track verbatim.
//!
//! There is no open-source VP6 *encoder* other than NihAV (ffmpeg only decodes
//! VP6), so this tool orchestrates NihAV rather than reimplementing the codec.

use std::path::PathBuf;
use std::process::Command;

const EA_DEFAULT_W: u32 = 1280;
const EA_DEFAULT_H: u32 = 720;
const EA_DEFAULT_FPS_NUM: u32 = 30000; // 30000/1001 = 29.97
const EA_DEFAULT_FPS_DEN: u32 = 1001;

struct Args {
    input: PathBuf,
    output: PathBuf,
    reference: Option<PathBuf>, // --match: copy dims/fps + reuse audio
    width: Option<u32>,
    height: Option<u32>,
    fps_num: Option<u32>,
    fps_den: Option<u32>,
    version: String, // vp60 | vp61 | vp62
    quant: Option<i32>,
    key_int: Option<u32>,
    no_audio: bool,
    ffmpeg: Option<PathBuf>,
    encoder: Option<PathBuf>,
    keep_temp: bool,
}

fn usage() -> ! {
    eprintln!(
        r#"vp6conv — convert a video into an NHL Legacy / EA VP6 (.vp6) movie.

USAGE:
    vp6conv <input-video> -o <output.vp6> [options]

COMMON:
    -o, --output <path>        Output .vp6 file (required).
    --match <ref.vp6>          Match a target movie: copy its width/height/fps AND
                               reuse its audio track verbatim. Recommended when
                               swapping out an existing movie so it keeps sound.

GEOMETRY (ignored fields default to NHL Legacy 1280x720 @ 29.97, or --match):
    --width <px>               Target width  (rounded down to a multiple of 16).
    --height <px>              Target height (rounded down to a multiple of 16).
    --fps <num/den>            Frame rate, e.g. 30000/1001 or 25/1.

ENCODE:
    --version <vp60|vp61|vp62> VP6 variant (default vp60 — NHL Legacy uses vp60).
    --quant <0-63>             Fixed quantiser (lower = better/larger; default auto).
    --key-int <n>              Keyframe interval in frames (default auto).
    --no-audio                 Force a silent (video-only) file even with --match.

TOOLS (auto-located in ./bin next to vp6conv, then PATH):
    --ffmpeg <path>            Path to ffmpeg.exe.
    --encoder <path>           Path to nihav-encoder.exe.
    --keep-temp                Keep intermediate Y4M / video-only files.

EXAMPLES:
    # Re-skin attract_eng, keep the original EA soundtrack:
    vp6conv myclip.mp4 -o attract_eng.vp6 --match path\to\attract_eng.vp6

    # Plain conversion to NHL Legacy defaults (silent):
    vp6conv intro.mov -o intro.vp6
"#
    );
    std::process::exit(2);
}

fn parse_args() -> Args {
    let mut a = Args {
        input: PathBuf::new(),
        output: PathBuf::new(),
        reference: None,
        width: None,
        height: None,
        fps_num: None,
        fps_den: None,
        version: "vp60".into(),
        quant: None,
        key_int: None,
        no_audio: false,
        ffmpeg: None,
        encoder: None,
        keep_temp: false,
    };
    let mut input_set = false;
    let mut output_set = false;
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        let mut next = || it.next().unwrap_or_else(|| {
            eprintln!("error: missing value for {arg}");
            usage()
        });
        match arg.as_str() {
            "-h" | "--help" => usage(),
            "-o" | "--output" => {
                a.output = PathBuf::from(next());
                output_set = true;
            }
            "--match" => a.reference = Some(PathBuf::from(next())),
            "--width" => a.width = Some(next().parse().unwrap_or_else(|_| die("bad --width"))),
            "--height" => a.height = Some(next().parse().unwrap_or_else(|_| die("bad --height"))),
            "--fps" => {
                let v = next();
                let (n, d) = parse_fps(&v);
                a.fps_num = Some(n);
                a.fps_den = Some(d);
            }
            "--version" => {
                a.version = next();
                if !matches!(a.version.as_str(), "vp60" | "vp61" | "vp62") {
                    die("--version must be vp60, vp61 or vp62");
                }
            }
            "--quant" => a.quant = Some(next().parse().unwrap_or_else(|_| die("bad --quant"))),
            "--key-int" => a.key_int = Some(next().parse().unwrap_or_else(|_| die("bad --key-int"))),
            "--no-audio" => a.no_audio = true,
            "--ffmpeg" => a.ffmpeg = Some(PathBuf::from(next())),
            "--encoder" => a.encoder = Some(PathBuf::from(next())),
            "--keep-temp" => a.keep_temp = true,
            s if s.starts_with('-') => die(&format!("unknown option {s}")),
            _ => {
                if !input_set {
                    a.input = PathBuf::from(arg);
                    input_set = true;
                } else {
                    die(&format!("unexpected extra argument {arg}"));
                }
            }
        }
    }
    if !input_set || !output_set {
        usage();
    }
    a
}

fn die(msg: &str) -> ! {
    eprintln!("error: {msg}");
    std::process::exit(1);
}

fn parse_fps(s: &str) -> (u32, u32) {
    if let Some((n, d)) = s.split_once('/') {
        (
            n.trim().parse().unwrap_or_else(|_| die("bad --fps numerator")),
            d.trim().parse().unwrap_or_else(|_| die("bad --fps denominator")),
        )
    } else {
        (s.trim().parse().unwrap_or_else(|_| die("bad --fps")), 1)
    }
}

/// Locate a helper exe: explicit flag, then ./bin next to vp6conv, then PATH.
fn locate(explicit: &Option<PathBuf>, exe: &str) -> PathBuf {
    if let Some(p) = explicit {
        return p.clone();
    }
    if let Ok(self_exe) = std::env::current_exe() {
        if let Some(dir) = self_exe.parent() {
            let cand = dir.join("bin").join(exe);
            if cand.exists() {
                return cand;
            }
            let cand2 = dir.join(exe);
            if cand2.exists() {
                return cand2;
            }
        }
    }
    PathBuf::from(exe) // rely on PATH
}

// ---- EA container parsing -------------------------------------------------

struct Chunk {
    tag: [u8; 4],
    start: usize, // offset of the 8-byte header
    size: usize,  // total bytes incl. the 8-byte header
}

fn rd_u16le(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}
fn rd_u32le(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

/// Parse the flat chunk list of an EA `MVhd` movie. Every chunk is
/// `[4-byte tag][u32 LE size incl. these 8 bytes][payload]`.
fn parse_chunks(b: &[u8]) -> Result<Vec<Chunk>, String> {
    let mut out = Vec::new();
    let mut off = 0usize;
    while off + 8 <= b.len() {
        let mut tag = [0u8; 4];
        tag.copy_from_slice(&b[off..off + 4]);
        let size = rd_u32le(b, off + 4) as usize;
        if size < 8 || off + size > b.len() {
            return Err(format!(
                "corrupt chunk at 0x{off:x}: tag={} size={size}",
                String::from_utf8_lossy(&tag)
            ));
        }
        out.push(Chunk { tag, start: off, size });
        off += size;
    }
    Ok(out)
}

fn is_video(tag: &[u8; 4]) -> bool {
    matches!(tag, b"MV0K" | b"MV0F" | b"AV0K" | b"AV0F")
}

struct MvHd {
    width: u32,
    height: u32,
    fps_num: u32, // value at offset 0x18 (tb_den)
    fps_den: u32, // value at offset 0x1C (tb_num)
}

fn read_mvhd(b: &[u8]) -> Result<MvHd, String> {
    if b.len() < 32 || &b[0..4] != b"MVhd" {
        return Err("not an MVhd movie (bad magic)".into());
    }
    Ok(MvHd {
        width: rd_u16le(b, 12) as u32,
        height: rd_u16le(b, 14) as u32,
        fps_num: rd_u32le(b, 24),
        fps_den: rd_u32le(b, 28),
    })
}

/// Build a final .vp6 = new MVhd header + new video frames re-interleaved over
/// the reference's audio chunks (verbatim), preserving the original A/V cadence.
fn splice_audio(reference: &[u8], video_only: &[u8]) -> Result<Vec<u8>, String> {
    let ref_chunks = parse_chunks(reference)?;
    let vid_chunks = parse_chunks(video_only)?;

    // New video frames, in order.
    let vid_frames: Vec<&Chunk> = vid_chunks.iter().filter(|c| is_video(&c.tag)).collect();
    if vid_frames.is_empty() {
        return Err("freshly-encoded file has no video frames".into());
    }
    // Reference video slots (one per original frame) and audio chunks, in stream order.
    let ref_video_slots = ref_chunks.iter().filter(|c| is_video(&c.tag)).count();
    if ref_video_slots == 0 {
        return Err("reference movie has no video frames to map onto".into());
    }
    if vid_frames.len() != ref_video_slots {
        eprintln!(
            "note: new clip has {} frames, reference has {} — mapping 1:1 and {}.",
            vid_frames.len(),
            ref_video_slots,
            if vid_frames.len() < ref_video_slots {
                "holding the last frame for the remainder"
            } else {
                "dropping the surplus"
            }
        );
    }

    // Header: start from the new video's MVhd (correct dims / frame-count / max
    // frame size for the new stream) but adopt the reference's fps fields so the
    // movie stays in sync with the reused audio.
    let mut header = video_only[0..32].to_vec();
    let ref_hdr = read_mvhd(reference)?;
    header[24..28].copy_from_slice(&ref_hdr.fps_num.to_le_bytes());
    header[28..32].copy_from_slice(&ref_hdr.fps_den.to_le_bytes());

    let mut out = Vec::with_capacity(video_only.len() + reference.len() / 4);
    out.extend_from_slice(&header);

    let mut vi = 0usize; // index into vid_frames
    for c in &ref_chunks {
        if &c.tag == b"MVhd" {
            continue; // replaced by our header above
        } else if is_video(&c.tag) {
            // Emit the next new frame (clamp-repeat the last if we ran short).
            let f = vid_frames[vi.min(vid_frames.len() - 1)];
            out.extend_from_slice(&video_only[f.start..f.start + f.size]);
            vi += 1;
        } else {
            // Audio (SC*) or anything else: copy verbatim.
            out.extend_from_slice(&reference[c.start..c.start + c.size]);
        }
    }
    Ok(out)
}

// ---- pipeline -------------------------------------------------------------

fn run(label: &str, cmd: &mut Command) {
    let status = cmd.status().unwrap_or_else(|e| die(&format!("failed to launch {label}: {e}")));
    if !status.success() {
        die(&format!("{label} failed (exit {:?})", status.code()));
    }
}

fn round16(v: u32) -> u32 {
    let r = v & !15;
    if r == 0 {
        16
    } else {
        r
    }
}

fn main() {
    let a = parse_args();
    if !a.input.exists() {
        die(&format!("input not found: {}", a.input.display()));
    }
    let ffmpeg = locate(&a.ffmpeg, "ffmpeg.exe");
    let encoder = locate(&a.encoder, "nihav-encoder.exe");

    // Resolve target geometry/fps: explicit flags > --match reference > defaults.
    let reference_bytes = match &a.reference {
        Some(p) => Some(std::fs::read(p).unwrap_or_else(|e| die(&format!("read --match: {e}")))),
        None => None,
    };
    let ref_hdr = reference_bytes.as_deref().map(|b| read_mvhd(b).unwrap_or_else(|e| die(&e)));

    let width = round16(a.width.or(ref_hdr.as_ref().map(|h| h.width)).unwrap_or(EA_DEFAULT_W));
    let height = round16(a.height.or(ref_hdr.as_ref().map(|h| h.height)).unwrap_or(EA_DEFAULT_H));
    let fps_num = a
        .fps_num
        .or(ref_hdr.as_ref().map(|h| h.fps_num))
        .unwrap_or(EA_DEFAULT_FPS_NUM);
    let fps_den = a
        .fps_den
        .or(ref_hdr.as_ref().map(|h| h.fps_den))
        .unwrap_or(EA_DEFAULT_FPS_DEN);

    eprintln!(
        "vp6conv: {} -> {}  ({}x{} @ {:.3} fps, {})",
        a.input.display(),
        a.output.display(),
        width,
        height,
        fps_num as f64 / fps_den as f64,
        a.version
    );

    // Temp files live next to the output.
    let stem = a
        .output
        .file_stem()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| "vp6conv".into());
    let dir = a.output.parent().filter(|p| !p.as_os_str().is_empty()).map(|p| p.to_path_buf()).unwrap_or_else(|| PathBuf::from("."));
    let y4m = dir.join(format!("{stem}.vp6conv.y4m"));
    let vid = dir.join(format!("{stem}.vp6conv.video.vp6"));

    // 1) ffmpeg: normalize to raw Y4M.
    run(
        "ffmpeg",
        Command::new(&ffmpeg).args([
            "-hide_banner",
            "-y",
            "-i",
        ]).arg(&a.input).args([
            "-an",
            "-vf",
            &format!("scale={width}:{height}:flags=lanczos,fps={fps_num}/{fps_den},format=yuv420p"),
            "-f",
            "yuv4mpegpipe",
        ]).arg(&y4m),
    );

    // 2) nihav-encoder: Y4M -> EA VP6 (video only).
    let mut ostream = format!("encoder=vp6,version={}", a.version);
    if let Some(q) = a.quant {
        ostream.push_str(&format!(",quant={q}"));
    }
    if let Some(k) = a.key_int {
        ostream.push_str(&format!(",key_int={k}"));
    }
    run(
        "nihav-encoder",
        Command::new(&encoder)
            .arg("--input").arg(&y4m)
            .arg("--output").arg(&vid)
            .args(["--output-format", "ea", "--ostream0", &ostream]),
    );

    // 3) Mux: splice reference audio, or copy the video-only file as-is.
    let want_audio = reference_bytes.is_some() && !a.no_audio;
    if want_audio {
        let vid_bytes = std::fs::read(&vid).unwrap_or_else(|e| die(&format!("read encoded video: {e}")));
        let out = splice_audio(reference_bytes.as_ref().unwrap(), &vid_bytes).unwrap_or_else(|e| die(&format!("audio splice: {e}")));
        std::fs::write(&a.output, &out).unwrap_or_else(|e| die(&format!("write output: {e}")));
        eprintln!("vp6conv: wrote {} (new video + reused audio), {} bytes", a.output.display(), out.len());
    } else {
        std::fs::copy(&vid, &a.output).unwrap_or_else(|e| die(&format!("write output: {e}")));
        let note = if a.reference.is_some() { " (--no-audio: silent)" } else { " (silent — pass --match <ref.vp6> to keep the original audio)" };
        eprintln!("vp6conv: wrote {}{}", a.output.display(), note);
    }

    if !a.keep_temp {
        let _ = std::fs::remove_file(&y4m);
        let _ = std::fs::remove_file(&vid);
    }
}
