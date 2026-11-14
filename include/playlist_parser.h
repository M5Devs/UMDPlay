#pragma once

// Parser for UMD Video / UMD Music PLAYLIST.UMD files.
//
// PLAYLIST.UMD is the top-level navigation table on a UMD disc. It lists the
// "tracks" (titles / playlists) the disc exposes and, for each, the clip file
// it plays plus a set of chapter markers. UMD Video uses it for the movie's
// chapter menu and the disc's title list; UMD Music uses it for the track list.
//
// The format is fully BIG-ENDIAN and uses a fixed 0x338-byte track record and
// 0x28-byte marker record (see docs/ARCHITECTURE.md for the verified layout).
//
// This module is deliberately pure (no SDL / FFmpeg): it takes a byte buffer
// and returns a parsed structure, so it is cheap to unit-test with synthetic
// and corrupt inputs. Every read is bounds-checked; a malformed file yields
// `ok() == false` (with a reason) rather than reading out of bounds.

#include <cstdint>
#include <string>
#include <vector>

namespace umdplay {

// One chapter / marker inside a track.
struct PlaylistMarker {
    uint8_t     flag = 0;          // entry type byte (0x05 = chapter marker)
    std::string name;              // label, e.g. "Mark4515" / "Chapter" (may be empty)
    uint32_t    timestamp_90k = 0; // raw PTS (90 kHz units)
    double      seconds = 0.0;     // timestamp_90k / 90000
};

// One track / playlist / title.
struct PlaylistTrack {
    std::string name;              // e.g. "3_Honpen"
    std::string clip_name;         // linked clip id, e.g. "00001" (no extension)
    uint32_t    start_90k = 0;     // IN timestamp (90 kHz)
    uint32_t    end_90k   = 0;     // OUT timestamp (90 kHz)
    double      start_seconds = 0.0;
    double      end_seconds   = 0.0;
    std::vector<PlaylistMarker> markers;
};

// The whole parsed playlist file.
struct Playlist {
    bool        valid = false;
    std::string error;             // reason when valid == false
    std::string magic;             // "UMDV0088"
    uint32_t    version = 0;       // the 4 ASCII digits after "UMDV"/"UMDA"
    std::string title;             // disc title from the file header
    std::vector<PlaylistTrack> tracks;

    // Convenience: the track that looks like the main feature (most markers,
    // longest span). nullptr when there are no tracks.
    const PlaylistTrack* main_track() const;

    // Chapters of a track as a flat, UI-friendly list (index + label + time).
    // Markers with an empty label get "Chapter N".
    struct Chapter {
        int         index = 0;      // 0-based
        std::string label;
        double      seconds = 0.0;
    };
    std::vector<Chapter> chapters(int track_index) const;
};

// Parse `data` (the raw PLAYLIST.UMD bytes). Never throws; on any structural
// problem returns a Playlist with valid=false and a human-readable error.
Playlist parse_playlist(const uint8_t* data, size_t size);

// --- Chapter navigation (pure; works on any list of {.seconds} entries) -----
//
// These mirror how DVD/BD players navigate: "previous" restarts the current
// chapter unless we are already near its start, in which case it steps back one.
// They are templates so both Playlist::Chapter (parser) and the UI's ChapterItem
// can be passed directly, and so the math is unit-testable without the UI.

// Index of the chapter containing `t` seconds: the last chapter whose start is
// <= t. Returns -1 when the list is empty or t precedes the first chapter.
template <class ChapterT>
int chapter_at(const std::vector<ChapterT>& chapters, double t) {
    int idx = -1;
    for (int i = 0; i < static_cast<int>(chapters.size()); ++i) {
        if (chapters[i].seconds <= t + 1e-6) idx = i;
        else break;
    }
    return idx;
}

// Next (+1) / previous (-1) chapter index relative to time `t`, or -1 when
// there is no such chapter. `restart_eps` is how far into the current chapter
// "previous" must be before it steps back instead of restarting.
template <class ChapterT>
int chapter_step(const std::vector<ChapterT>& chapters, double t, int dir,
                 double restart_eps = 1.5) {
    if (chapters.empty()) return -1;
    const int cur = chapter_at(chapters, t);
    if (dir < 0) {
        if (cur < 0) return -1;
        const int target = (t > chapters[cur].seconds + restart_eps) ? cur : cur - 1;
        return target >= 0 ? target : -1;
    }
    const int target = (cur < 0) ? 0 : cur + 1;
    return (target < static_cast<int>(chapters.size())) ? target : -1;
}

// Probe summary for --probe: a short multi-line human-readable description of
// the file (header fields + track/marker counts). Never throws.
std::string playlist_summary(const Playlist& pl);

}  // namespace umdplay
