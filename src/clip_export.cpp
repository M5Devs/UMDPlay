#include "clip_export.h"
#include "decoder.h"

#if defined(_WIN32)
#include <process.h>   // _spawnvp / _P_WAIT
#include <windows.h>   // SearchPathA
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Scratch directory for the raw intermediates. /tmp does not exist on Windows;
// the OS temp directory is correct on both platforms.
std::string temp_dir() {
    std::error_code ec;
    fs::path p = fs::temp_directory_path(ec);
    if (ec || p.empty()) return ".";
    return p.string();
}

// Per-process id, so two concurrent umdplay runs never share a scratch file.
unsigned long process_id() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::_getpid());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

std::string temp_path(const std::string& tag) {
    const std::string stem =
        "umdplay_" + tag + "_" + std::to_string(process_id());
    return (fs::path(temp_dir()) / (stem + ".raw")).string();
}

// The encoder is the ffmpeg CLI, never a linked library. When it is missing the
// export degrades to the raw intermediates instead of failing.
bool ffmpeg_available() {
#if defined(_WIN32)
    char found[MAX_PATH];
    return ::SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, found, nullptr) > 0;
#else
    const char* path = std::getenv("PATH");
    if (!path) return false;
    std::string dirs(path);
    size_t start = 0;
    while (start <= dirs.size()) {
        const size_t end = dirs.find(':', start);
        const std::string dir = dirs.substr(start, end == std::string::npos
                                                       ? std::string::npos
                                                       : end - start);
        if (!dir.empty() && ::access((dir + "/ffmpeg").c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
#endif
}

bool run_ffmpeg(const std::string& out_path, int vw, int vh, double fps,
                double duration, const std::string& video_raw,
                const std::string& audio_raw) {
    char fps_buf[32], dur_buf[32], size_buf[32];
    std::snprintf(fps_buf, sizeof(fps_buf), "%f", fps);
    std::snprintf(dur_buf, sizeof(dur_buf), "%f", duration);
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", vw, vh);

    const char* argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pixel_format", "yuv420p",
        "-video_size", size_buf, "-framerate", fps_buf,
        "-i", video_raw.c_str(),
        "-f", "s16le", "-ar", "44100", "-ac", "2",
        "-i", audio_raw.c_str(),
        "-t", dur_buf,
        "-c:v", "libx264", "-preset", "medium", "-crf", "20",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac", "-b:a", "192k",
        "-movflags", "+faststart",
        out_path.c_str(),
        nullptr
    };

#if defined(_WIN32)
    // No fork/exec on Windows: spawn ffmpeg and wait for it to exit.
    const intptr_t rc = _spawnvp(_P_WAIT, "ffmpeg", argv);
    return rc == 0;
#else
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execvp("ffmpeg", const_cast<char* const*>(argv));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

// Write the two raw intermediates next to `out_path` when ffmpeg is absent.
// A 44-byte canonical WAV header keeps the audio playable on its own.
bool write_raw_pair(const std::string& out_path, const std::string& video_raw,
                    const std::string& audio_raw) {
    const fs::path base(out_path);
    const std::string yuv_path = base.string() + ".yuv";
    const std::string wav_path = base.string() + ".wav";
    std::error_code ec;
    fs::copy_file(video_raw, yuv_path, fs::copy_options::overwrite_existing, ec);
    if (ec) return false;

    std::ifstream in(audio_raw, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto audio_size = static_cast<uint32_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    std::ofstream wav(wav_path, std::ios::binary);
    if (!wav) return false;
    const uint32_t riff_size = 36 + audio_size;
    const uint32_t byte_rate = 44100 * 2 * 2;
    wav.write("RIFF", 4);
    wav.write(reinterpret_cast<const char*>(&riff_size), 4);
    wav.write("WAVEfmt ", 8);
    const uint32_t fmt_size = 16;
    const uint16_t fmt_pcm = 1, channels = 2, block_align = 4, bits = 16;
    const uint32_t rate = 44100;
    wav.write(reinterpret_cast<const char*>(&fmt_size), 4);
    wav.write(reinterpret_cast<const char*>(&fmt_pcm), 2);
    wav.write(reinterpret_cast<const char*>(&channels), 2);
    wav.write(reinterpret_cast<const char*>(&rate), 4);
    wav.write(reinterpret_cast<const char*>(&byte_rate), 4);
    wav.write(reinterpret_cast<const char*>(&block_align), 2);
    wav.write(reinterpret_cast<const char*>(&bits), 2);
    wav.write("data", 4);
    wav.write(reinterpret_cast<const char*>(&audio_size), 4);
    wav << in.rdbuf();
    std::cout << "[+] clip_export: ffmpeg not found; wrote " << yuv_path
              << " (YUV420P) and " << wav_path << " (S16LE 44.1 kHz stereo)\n";
    return true;
}

// Write one YUV420P picture as tight planes (what ffmpeg's rawvideo wants).
void write_yuv420p(std::ofstream& out, const umdplay::VideoFrame& f) {
    for (int p = 0; p < 3; ++p) {
        const int pw = p ? f.chroma_width() : f.width;
        const int ph = p ? f.chroma_height() : f.height;
        for (int y = 0; y < ph; ++y) {
            out.write(reinterpret_cast<const char*>(f.plane(p) + static_cast<size_t>(y) * f.stride[p]),
                      pw);
        }
    }
}

}  // namespace

bool export_clip(const std::string& iso_path,
                 const MpsEntry&    entry,
                 double             start_seconds,
                 double             duration_seconds,
                 const std::string& out_path) {
    Decoder decoder;
    if (!decoder.open_stream(iso_path, entry)) {
        std::cerr << "[-] clip_export: cannot open stream " << entry.name << "\n";
        return false;
    }

    const double fps = (decoder.get_fps() > 1.0 && decoder.get_fps() < 121.0)
                           ? decoder.get_fps() : 29.97;
    const int vw = decoder.video_width();
    const int vh = decoder.video_height();
    if (vw <= 0 || vh <= 0) {
        std::cerr << "[-] clip_export: stream has no usable video dimensions ("
                  << (decoder.has_video() ? decoder.video_backend_name() : "no video backend")
                  << ", " << vw << "x" << vh << ")\n";
        decoder.close();
        return false;
    }
    std::cout << "[*] clip_export: video=" << decoder.video_codec_name()
              << " (" << decoder.video_backend_name() << ") "
              << vw << "x" << vh << " @" << fps << "fps, audio="
              << (decoder.has_audio()
                      ? "ATRAC3+ (native PsmfDemuxer 0xBD)"
                      : "none")
              << " " << decoder.audio_sample_rate() << "Hz\n";
    if (start_seconds > 0.0) {
        std::cout << "[*] clip_export: seeking to " << start_seconds << "s\n";
        decoder.seek_absolute(start_seconds);
    }

    // Demux to raw intermediate files. ffmpeg reads both inputs interleaved, so
    // piping them directly would deadlock (ffmpeg blocks on audio while we block
    // writing video); files make the encode a separate, simple step.
    const std::string video_raw = temp_path("v");
    const std::string audio_raw = temp_path("a");
    std::ofstream vf(video_raw, std::ios::binary);
    std::ofstream af(audio_raw, std::ios::binary);
    if (!vf || !af) {
        std::cerr << "[-] clip_export: cannot create temp files\n";
        decoder.close();
        return false;
    }

    umdplay::VideoFrame frame;
    std::vector<uint8_t> pcm;

    const double total_frames = duration_seconds * fps;
    double video_frames = 0.0;
    size_t audio_bytes = 0;
    int    interlaced_frames = 0;

    while (video_frames < total_frames) {
        pcm.clear();
        const FrameType ft = decoder.read_frame(frame, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            if (decoder.last_frame_interlaced()) ++interlaced_frames;
            write_yuv420p(vf, frame);
            ++video_frames;
        } else if (ft == FrameType::Audio && !pcm.empty()) {
            af.write(reinterpret_cast<const char*>(pcm.data()),
                     static_cast<std::streamsize>(pcm.size()));
            audio_bytes += pcm.size();
        }
    }

    decoder.close();
    vf.close();
    af.close();

    std::cout << "[*] clip_export: decoded " << static_cast<int>(video_frames)
              << " video frames (" << interlaced_frames << " field-coded), "
              << audio_bytes << " audio bytes\n";

    if (video_frames < 1) {
        std::cerr << "[-] clip_export: no video frames decoded\n";
        std::remove(video_raw.c_str());
        std::remove(audio_raw.c_str());
        return false;
    }

    // Trim the clip to the frames actually decoded (a seek can shorten it).
    const double actual = video_frames / fps;
    bool ok = false;
    if (ffmpeg_available()) {
        ok = run_ffmpeg(out_path, vw, vh, fps, actual, video_raw, audio_raw);
        if (!ok) std::cerr << "[-] clip_export: ffmpeg encode failed\n";
    } else {
        ok = write_raw_pair(out_path, video_raw, audio_raw);
    }

    std::remove(video_raw.c_str());
    std::remove(audio_raw.c_str());
    return ok;
}
