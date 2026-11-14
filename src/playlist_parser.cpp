#include "playlist_parser.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace umdplay {

namespace {

// Bounds-checked big-endian readers. Each returns false when the requested
// bytes fall outside the buffer, so a truncated/corrupt file can never read
// out of bounds.
bool be16(const uint8_t* d, size_t n, size_t off, uint16_t& out) {
    if (off + 2 > n) return false;
    out = static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
    return true;
}
bool be32(const uint8_t* d, size_t n, size_t off, uint32_t& out) {
    if (off + 4 > n) return false;
    out = (static_cast<uint32_t>(d[off]) << 24) |
          (static_cast<uint32_t>(d[off + 1]) << 16) |
          (static_cast<uint32_t>(d[off + 2]) << 8) |
          static_cast<uint32_t>(d[off + 3]);
    return true;
}

// Read a NUL-terminated (or length-prefixed) ASCII string at `off`, capped at
// `maxlen` bytes. Returns "" when out of range.
std::string fixed_str(const uint8_t* d, size_t n, size_t off, size_t maxlen) {
    if (off >= n) return {};
    size_t end = std::min(n, off + maxlen);
    size_t len = 0;
    while (off + len < end && d[off + len] != 0) ++len;
    return std::string(reinterpret_cast<const char*>(d + off), len);
}

constexpr size_t kHeaderSize      = 0x130;   // file data starts here
constexpr size_t kTrackRecordSize = 0x338;   // fixed track entry size
constexpr size_t kMarkerRecordSize = 0x28;   // fixed marker entry size
constexpr size_t kMaxTrackName    = 0x2DB;   // name field length inside a track
constexpr size_t kMaxMarkers      = 4096;    // sanity cap per track

}  // namespace

const PlaylistTrack* Playlist::main_track() const {
    if (tracks.empty()) return nullptr;
    const PlaylistTrack* best = &tracks.front();
    auto score = [](const PlaylistTrack& t) {
        return t.markers.size() * 1000ull +
               static_cast<unsigned long long>(t.end_seconds - t.start_seconds);
    };
    for (const auto& t : tracks) if (score(t) > score(*best)) best = &t;
    return best;
}

std::vector<Playlist::Chapter> Playlist::chapters(int track_index) const {
    std::vector<Chapter> out;
    if (track_index < 0 || track_index >= static_cast<int>(tracks.size()))
        return out;
    const auto& t = tracks[track_index];
    out.reserve(t.markers.size());
    for (size_t i = 0; i < t.markers.size(); ++i) {
        Chapter c;
        c.index   = static_cast<int>(i);
        c.seconds = t.markers[i].seconds;
        c.label   = t.markers[i].name;
        if (c.label.empty()) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Chapter %zu", i + 1);
            c.label = buf;
        }
        out.push_back(std::move(c));
    }
    return out;
}

Playlist parse_playlist(const uint8_t* data, size_t size) {
    Playlist pl;
    if (!data || size < kHeaderSize) {
        pl.error = "file too small for a PLAYLIST.UMD header";
        return pl;
    }

    pl.magic.assign(reinterpret_cast<const char*>(data), 8);

    // Accept both "UMDV0088" (video) and "UMDA0088" (audio) style magic.
    const bool is_video = pl.magic.rfind("UMDV", 0) == 0;
    const bool is_audio = pl.magic.rfind("UMDA", 0) == 0;
    if (!is_video && !is_audio) {
        pl.error = "bad magic (expected UMDV/UMDA)";
        return pl;
    }
    // Version = the 4 ASCII digits following the 4-letter prefix.
    pl.version = 0;
    for (int i = 4; i < 8; ++i) {
        if (data[i] < '0' || data[i] > '9') { pl.error = "bad version digits"; return pl; }
        pl.version = pl.version * 10 + static_cast<uint32_t>(data[i] - '0');
    }

    // Disc title: length byte at 0x30, then the text.
    if (data[0x30] > 0) {
        pl.title = fixed_str(data, size, 0x31, data[0x30]);
    }

    // File-data table at 0x130: dword offset-to-EOF, word track count.
    uint32_t data_off = 0;
    if (!be32(data, size, 0x130, data_off)) {
        pl.error = "truncated file-data table";
        return pl;
    }
    uint16_t ntracks = 0;
    if (!be16(data, size, 0x134, ntracks)) {
        pl.error = "truncated track count";
        return pl;
    }
    // The data table should fit inside the file (offset from 0x134 to EOF).
    if (static_cast<uint64_t>(0x134) + data_off > size + kTrackRecordSize) {
        // Not fatal (some discs pad), but a wildly wrong value is corruption.
        if (data_off > size) { pl.error = "file-data offset beyond EOF"; return pl; }
    }

    size_t off = 0x136;
    for (uint16_t t = 0; t < ntracks; ++t) {
        // Track record header: dword entry length (excluding these 4 bytes).
        uint32_t ent_len = 0;
        if (!be32(data, size, off, ent_len)) { pl.error = "truncated track entry"; return pl; }
        if (ent_len < kTrackRecordSize - 4 || ent_len > size) {
            pl.error = "implausible track entry length";
            return pl;
        }
        const size_t ent_end = off + 4 + ent_len;
        if (ent_end > size) { pl.error = "track entry extends past EOF"; return pl; }

        PlaylistTrack track;
        track.name      = fixed_str(data, size, off + 0x37, kMaxTrackName);
        track.clip_name = fixed_str(data, size, off + 0x31E, 8);
        be32(data, size, off + 0x328, track.start_90k);
        be32(data, size, off + 0x32E, track.end_90k);
        track.start_seconds = track.start_90k / 90000.0;
        track.end_seconds   = track.end_90k   / 90000.0;

        uint16_t nmark = 0;
        be16(data, size, off + 0x336, nmark);
        if (nmark > kMaxMarkers) { pl.error = "implausible marker count"; return pl; }

        // Markers begin right after the fixed track record.
        size_t mo = off + kTrackRecordSize;
        for (uint16_t m = 0; m < nmark; ++m) {
            if (mo + kMarkerRecordSize > ent_end) break;  // clamp to entry bounds
            PlaylistMarker mk;
            mk.flag = data[mo];
            uint32_t ts = 0;
            be32(data, size, mo + 6, ts);
            mk.timestamp_90k = ts;
            mk.seconds = ts / 90000.0;
            // Name field: 0x18 bytes of ASCII after the 0x10-byte marker head.
            mk.name = fixed_str(data, size, mo + 0x10, 0x18);
            track.markers.push_back(std::move(mk));
            mo += kMarkerRecordSize;
        }

        pl.tracks.push_back(std::move(track));
        off = ent_end;
    }

    pl.valid = true;
    return pl;
}

std::string playlist_summary(const Playlist& pl) {
    char buf[256];
    std::string out;
    if (!pl.valid) {
        out = "PLAYLIST.UMD: INVALID (" + pl.error + ")\n";
        return out;
    }
    std::snprintf(buf, sizeof(buf), "PLAYLIST.UMD: magic=%s version=%u tracks=%zu title=\"%s\"\n",
                  pl.magic.c_str(), pl.version, pl.tracks.size(), pl.title.c_str());
    out += buf;
    for (size_t i = 0; i < pl.tracks.size(); ++i) {
        const auto& t = pl.tracks[i];
        const int sm = static_cast<int>(t.start_seconds) / 60;
        const int ss = static_cast<int>(t.start_seconds) % 60;
        const int em = static_cast<int>(t.end_seconds) / 60;
        const int es = static_cast<int>(t.end_seconds) % 60;
        std::snprintf(buf, sizeof(buf),
                      "  [%zu] %-12s clip=%-5s  %02d:%02d..%02d:%02d  markers=%zu\n",
                      i, t.name.c_str(), t.clip_name.c_str(),
                      sm, ss, em, es, t.markers.size());
        out += buf;
    }
    return out;
}

}  // namespace umdplay
