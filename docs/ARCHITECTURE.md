# UMDPlay — Architecture Notes

## UMD Video ISO Layout (Verified)

UMD Video discs use **UDF 1.02** overlaid with an ISO 9660 compatibility table.
The ISO 9660 PVD at sector 16 is an intentional **dummy** — its root directory has `size=22 bytes` and cannot be used for direct filesystem traversal.

### Real Disc Layout & Sector Map

```text
Sector 16     : ISO 9660 PVD       (dummy — root dir size=22, ignore)
Sectors 18-19 : UDF Path Table LE  (UMD_VIDEO @ LBA 24)
Sectors 20-21 : UDF Path Table BE  (UMD_VIDEO @ LBA 24, big-endian)
Sector  22    : UDF Volume Set Descriptor
Sector  24    : UMD_VIDEO/ ICB (File Entry)
Sector  28    : STREAM/ Directory Records (ISO 9660 format inside UDF)
Sector  144   : CLIPINF/00001.CLP  (Magic: CLPI0088 — Clip Information v0.88)
LBA  272      : 00099.MPS
LBA  288      : 00098.MPS
LBA  592      : 00002.MPS
LBA 2880      : 00001.MPS          (Main Feature — 1.72 GB, Pack Start: 00 00 01 BA ✅)
```

Verified against: *Miku no Hi 39's Giving Day — Project DIVA [UMD VIDEO]*

---

## Directory Record Structure (STREAM/ Sector 28)

Records inside Sector 28 follow the standard **ISO 9660 Directory Record** structure (Record length = `0x38` / 56 bytes for standard entries).

Relative to `name_start`:
```text
Offset             Size    Field
[name_start - 31]  4       LBA of file data (uint32 LE)
[name_start - 27]  4       LBA of file data (uint32 BE)
[name_start - 23]  4       File size in bytes (uint32 LE)
[name_start - 19]  4       File size in bytes (uint32 BE)
[name_start - 1]   1       Filename length
[name_start ..]    N       Filename string (ASCII, e.g. "00001.MPS")
```

---

## Media Container & Demuxing (.MPS)

`.MPS` files are **MPEG-2 Program Streams (MPEG-PS)** (ISO/IEC 13818-1), but with proprietary Sony packetizations for audio and clip navigation.

- **Magic Bytes:** `00 00 01 BA` (Pack Header)
- **Video Track:** H.264 / AVC Main Profile @ Level 3 (Stream ID `0x1E0`, 720×480 @ 29.97 fps)
- **Audio Track:** ATRAC3plus packed inside **Private Stream 1 (`0x000001BD`)**

### In-Memory Streaming (Phase 2 Solution)
To avoid extracting gigabytes of data to disk, `UMDPlay` feeds raw sector streams directly into FFmpeg via a custom `AVIOContext` with seekable callbacks:
`Base_Byte_Offset = entry.lba * 2048`.

---

## The Sony PSMF Audio Structure (Reverse-Engineered)

Generic media players fail to play audio because Sony omits standard PSMF file headers on UMD Video discs, packing ATRAC3plus frames inside continuous PES packets across Private Stream 1 (`0xBD`).

### 1. PES Audio Header & Access Unit Pointers
Every `0x000001BD` PES packet payload begins with a 4-byte sub-header:
```text
Byte 0..1 : Stream / Substream flags (0x00 0x00)
Byte 2..3 : First Access Unit Pointer (uint16 BE)
```
- Audio frames **span across PES packet boundaries**.
- The 2-byte pointer indicates the byte offset where the next complete audio frame begins within that packet payload.

### 2. The 8-Byte Frame Header
Every ATRAC3plus frame is prepended with an 8-byte proprietary Sony frame sync header:
```text
0F D0 48 55 00 00 00 00
```
Stripping this 8-byte prefix exposes the pure ATRAC3plus elementary frame payload.

### 3. Bitstream Decoding via FFmpeg
The elementary ATRAC3plus stream can be fed into FFmpeg's `atrac3p` decoder by wrapping the frames inside a `WAVE_FORMAT_EXTENSIBLE` (Format Tag `0xFFFE`) container with the official ATRAC3plus GUID:
```c
{ 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 }
```
- **Sample Rate:** 44,100 Hz
- **Channels:** 2 (Stereo)
- **Samples Per Block:** 2,048

---

## Clip Information (.CLP)

Files inside `UMD_VIDEO/CLIPINF/` (e.g. `00001.CLP`) use the **`CLPI0088`** container (an early predecessor to Blu-ray's `CLPI0100` format).
- Offset `0x08`: Pointer to `SequenceInfo`
- Offset `0x0C`: Pointer to `ProgramInfo` (stores video/audio stream attributes, codecs, and language flags)

---

## Playlist & Chapters (`PLAYLIST.UMD`)

`UMD_VIDEO/PLAYLIST.UMD` is the disc's top-level navigation table: the title
list, the clip each title plays, and the chapter markers the movie player
exposes. The whole file is **big-endian** (unlike the little-endian `.RCO`).

Header (`0x00`–`0x135`):

| Offset | Size | Field |
|---|---|---|
| `0x00` | 8 | Magic `UMDV0088` (`UMDA0088` on UMD Audio); the 4 ASCII digits are the version |
| `0x08` | 4 | Offset to the file-data table (`0x130` on the sample disc) |
| `0x2C` | 4 | Number of tracks (also re-stated at `0x134`) |
| `0x30` | 1 | Title length, then the title bytes (e.g. `UMDV-SAMPLE`) |

File-data table at `0x130`: `+0x00` = 4-byte size from `0x134` to EOF;
`+0x04` = 2-byte track count; then the track records.

Each **track record** (`0x338` bytes, plus `0x28` bytes per marker):

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | Entry length (bytes following this field) |
| `0x36` | 1+N | Track name (e.g. `3_Honpen`) |
| `0x312` | 2 | Flags |
| `0x31E` | 5 | Linked clip id (e.g. `00001` → `STREAM/00001.MPS`) |
| `0x328` | 4 | IN timestamp (90 kHz units) |
| `0x32E` | 4 | OUT timestamp (90 kHz units) |
| `0x336` | 2 | Marker count |

Each **marker record** (`0x28` bytes): `+0x00` type byte (`0x05` = chapter),
`+0x01` label length, `+0x06` timestamp (90 kHz), `+0x10` label (e.g.
`Mark4515`). The name is a label, not a number — the numeric suffix is only a
convention; the authoritative chapter time is the 90 kHz timestamp
(`seconds = ts / 90000`).

On the reference disc the main feature is track `3_Honpen` (clip `00001`) with
**45 markers**; the parser round-trips its track table to EOF byte-exactly.

`include/playlist_parser.h` / `src/playlist_parser.cpp` expose `parse_playlist()`
(pure, bounds-checked, never throws), `Playlist::main_track()`,
`Playlist::chapters()`, plus the pure navigation helpers `chapter_at()` and
`chapter_step()` used by both the CLI and the UI.

---

## Menu Resources (`RESOURCE/*.RCO`)

Files inside `UMD_VIDEO/RESOURCE/` (e.g. `JA100000.RCO`) are the interactive
menu resources. The container is **little-endian** with a leading NUL byte:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 4 | Magic `\0PRF` |
| `0x04` | 4 | Version (`0x80`) |
| `0x10` | 8 | TOC entry 0: 4-byte offset + 4-byte size (LE); `0xFFFFFFFF` = absent |
| … | 8 | Further TOC entries |

Embedded resources are located by the TOC. The reference file holds **81 PNG
images** (buttons/labels for the menu) and a **`VSMX`** bytecode block (the
menu's script VM). `include/rco_parser.h` / `src/rco_parser.cpp` probe the
header/TOC, enumerate the PNGs (with dimensions), and report the `VSMX` offset.

---

## Phase Roadmap

| Phase | Status | Description |
|---|---|---|
| 1 | ✅ Done | UDF ISO parser — locate & read .MPS stream sectors |
| 2 | ✅ Done | FFmpeg custom AVIO streaming + H.264 video decoding + ATRAC3+ PSMF audio reverse-engineering |
| 3 | ✅ Done | Standalone SDL2 + ImGui GUI player with synced A/V playback |
| 4 | 🔄 In progress | Interactive menus & chapters — PLAYLIST.UMD / CLPI0088 / `.RCO` parsing (done), real chapter navigation (done), `.VBO` / VSMX menu VM (planned) |
| 5 | ⏳ Planned | PPSSPP integration API |
