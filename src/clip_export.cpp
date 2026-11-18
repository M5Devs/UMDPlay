#include "clip_export.h"
#include "decoder.h"

#if defined(_WIN32)
#include <process.h>   // _spawnvp / _P_WAIT
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libswscale/swscale.h>
}

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

bool run_ffmpeg(const std::string& out_path, int vw, int vh, double fps,
                double duration, const std::string& video_raw,
                const std::string& audio_raw) {
    char fps_buf[32], dur_buf[32], size_buf[32];
    std::snprintf(fps_buf, sizeof(fps_buf), "%f", fps);
    std::snprintf(dur_buf, sizeof(dur_buf), "%f", duration);
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", vw, vh);

    const char* argv[] = {
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pixel_format", "rgb24",
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
        std::cerr << "[-] clip_export: stream has no usable video dimensions\n";
        decoder.close();
        return false;
    }
    std::cout << "[*] clip_export: video=" << decoder.video_codec_name()
              << " " << vw << "x" << vh << " @" << fps << "fps, audio="
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

    SwsContext* sws = sws_getContext(vw, vh, AV_PIX_FMT_YUV420P,
                                     vw, vh, AV_PIX_FMT_RGB24,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        std::cerr << "[-] clip_export: sws_getContext failed\n";
        decoder.close();
        return false;
    }

    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> rgb(static_cast<size_t>(vw) * vh * 3);
    uint8_t* dst[4] = { rgb.data(), nullptr, nullptr, nullptr };
    int dst_linesize[4] = { vw * 3, 0, 0, 0 };
    std::vector<uint8_t> pcm;

    const double total_frames = duration_seconds * fps;
    double video_frames = 0.0;
    size_t audio_bytes = 0;

    while (video_frames < total_frames) {
        pcm.clear();
        const FrameType ft = decoder.read_frame(frame, pcm);
        if (ft == FrameType::Eof) break;
        if (ft == FrameType::Video) {
            sws_scale(sws, frame->data, frame->linesize, 0, vh, dst, dst_linesize);
            vf.write(reinterpret_cast<const char*>(rgb.data()),
                     static_cast<std::streamsize>(rgb.size()));
            ++video_frames;
        } else if (ft == FrameType::Audio && !pcm.empty()) {
            af.write(reinterpret_cast<const char*>(pcm.data()),
                     static_cast<std::streamsize>(pcm.size()));
            audio_bytes += pcm.size();
        }
    }

    av_frame_free(&frame);
    sws_freeContext(sws);
    decoder.close();
    vf.close();
    af.close();

    std::cout << "[*] clip_export: decoded " << static_cast<int>(video_frames)
              << " video frames, " << audio_bytes << " audio bytes\n";

    if (video_frames < 1) {
        std::cerr << "[-] clip_export: no video frames decoded\n";
        std::remove(video_raw.c_str());
        std::remove(audio_raw.c_str());
        return false;
    }

    // Trim the clip to the frames actually decoded (a seek can shorten it).
    const double actual = video_frames / fps;
    const bool ok = run_ffmpeg(out_path, vw, vh, fps, actual, video_raw, audio_raw);
    if (!ok) std::cerr << "[-] clip_export: ffmpeg encode failed\n";

    std::remove(video_raw.c_str());
    std::remove(audio_raw.c_str());
    return ok;
}
