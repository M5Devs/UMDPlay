#pragma once

#include "iso_reader.h"

#include <string>

// Decode a short A/V segment from a UMD Video .MPS stream and mux it into a
// standard MP4 via an ffmpeg child process (H.264 video + AAC audio).
//
// The decoder emits raw RGB24 video frames and S16LE stereo PCM; both are piped
// to ffmpeg over dedicated file descriptors (fd 3 = video, fd 4 = audio), which
// keeps the export free of any extra muxing/encoding dependency.
//
// Returns true on success. Requires the `ffmpeg` CLI on PATH.
bool export_clip(const std::string& iso_path,
                 const MpsEntry&    entry,
                 double             start_seconds,
                 double             duration_seconds,
                 const std::string& out_path);
