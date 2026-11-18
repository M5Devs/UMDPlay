# UMDPlay — Architecture Notes

This document records what UMDPlay has **reverse-engineered and verified** about
UMD disc internals, plus the design of the parsers that read them. Everything
below was confirmed against real discs (a Japanese UMD Video concert disc and a
US UMD Video film) unless marked otherwise.

> Compressing a UMD ISO to a `.chd`? See the dedicated
> [CHD Compression Guide](CHD_GUIDE.md) — using the wrong `chdman` mode silently
> breaks the disc.

---

## UMD Disc Families

A UMD is a **1.8 GB optical disc** that can carry several kinds of content. The
PSP XMB (and UMDPlay) distinguish them by the disc's `PARAM.SFO` `CATEGORY` code
and by which content tree exists at the volume root. The standard PSP category
codes are:

| CATEGORY | Disc type | Root tree | Contents |
|---|---|---|---|
| `UV` | **UMD Video** | `UMD_VIDEO/` | Movie/TV `.MPS` streams, `PLAYLIST.UMD`, `.RCO` menus |
| `UA` | **UMD Audio** (UMD Music) | `UMD_AUDIO/` | Audio tracks + a `PLAYLIST.UMD` track list (`UMDA0088`) |
| `UG` | **UMD Game** | `PSP_GAME/` | PSP game `EBOOT.BIN` + `PSP_GAME/PARAM.SFO` |
| `MG` | Memory Stick Game | `PSP_GAME/` | Game installed on a Memory Stick (no UMD) |

(Not every disc ships a `CATEGORY` key — the reference UMD Video disc carries
only `TITLE`. UMDPlay therefore keys off the content tree, not the category.)

**UMD Video** is the primary target of this project: it uses the
`UMD_VIDEO/` tree (`STREAM/`, `CLIPINF/`, `PLAYLIST.UMD`, `RESOURCE/`).

### UMD-PG (interactive "player's game")

**UMD-PG** is a rare variant used for interactive visual novels/eroge released
on UMD. Despite behaving like a game, a UMD-PG disc **is a UMD Video disc**: it
carries a `UMD_VIDEO/` tree and presents as `CATEGORY UV`, which is exactly why
it exists — it avoids the *video game* age-rating bodies (CERO/ESRB) by shipping
as a "video" rather than a game. Its interactivity is delivered by the disc's
own **`PLAYLIST.UMD` + `.RCO`/VSMX menu system** (the same machinery a normal
UMD Video menu uses), not by a `PSP_GAME/EBOOT.BIN` executable, and it keeps no
save data. So the UMD Video parser in this project covers UMD-PG discs as well.

---

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

## Disc Metadata (`PARAM.SFO`)

The volume root carries a Sony **SFO** ("System File Object") metadata block,
`PARAM.SFO`, used by the PSP XMB to display and categorise the disc. The format
is little-endian with the magic `\0PSF`:

```text
Offset  Size  Field
0x00    4     Magic "\0PSF"
0x04    4     Version (0x00000101)
0x08    4     Absolute offset of the key table
0x0C    4     Absolute offset of the data table
0x10    4     Entry count (index table entries)
0x14    N*16  Index table: key offset(u16) fmt(u16) len(u32) max_len(u32) data offset(u32)
...           Key table: NUL-terminated ASCII keys, alphabetical
...           Data table: values, indexed by the entries above
```

Field formats: `0x0402` = NUL-terminated UTF-8, `0x0404` = little-endian
`uint32`, `0x0004` = raw bytes. UMDPlay reads `TITLE`, `DISC_ID`, `CATEGORY`
and `APP_VER`; the `CATEGORY` value is what identifies the disc family (see
[UMD Disc Families](#umd-disc-families)). `include/param_sfo.h` /
`src/param_sfo.cpp` implement a bounds-checked `parse_param_sfo()` that never
reads past the buffer and returns empty fields on a malformed block.

The disc's cover art lives alongside it as `ICON0.PNG` (hub cover) and
`PIC1.PNG` (background), decoded with `stb_image`.

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
To avoid extracting gigabytes of data to disk, `UMDPlay` reads raw sector streams
directly through a `DiscReader` and demuxes them in memory with a native
`PsmfDemuxer` — **no `libavformat`/`AVFormatContext`/`AVIOContext`**. Only the
elementary streams are handed to FFmpeg `libavcodec` for decoding.

(The shared `libavformat.so` may still appear in `ldd` on Debian/Ubuntu because
its `libavfilter.so` — used only for optional yadif — is built with a `NEEDED`
entry for it. UMDPlay neither links nor calls it; a build without `libavfilter`
has no `libavformat` in the process at all.)

`Base_Byte_Offset = entry.lba * 2048`.

The bytes themselves come from a `DiscReader` (`include/disc_reader.h`) — a thin
byte-range abstraction with two backends selected by file extension:

- **`iso`** — raw 2048-byte-sector images read with a plain positional read.
- **`chd`** — MAME *Compressed Hunks of Data* files decoded through
  [libchdr](https://github.com/rtissera/libchdr) (fetched and statically linked
  by CMake). CHDs are stored in 4096-byte hunks while UMD sectors are 2048
  bytes, so `ChdBackend` keeps a small direct-mapped cache of decompressed
  hunks and serves unaligned cross-hunk reads from it.

`IsoReader`, `PsmfDemuxer`, `Decoder` and the clip exporter all read through this
one interface, so `.iso` and `.chd` behave identically — verified by exporting
byte-identical MP4 clips from an ISO and its CHD twin.

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
The elementary ATRAC3plus stream is fed into FFmpeg's `atrac3p` decoder by
setting `AVCodecContext::block_align` to the measured stride and passing each
8-byte-prefix-stripped frame as one packet. (Sony's `WAVE_FORMAT_EXTENSIBLE`
GUID wrapper — `{ 0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19,
0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62 }` — is not needed with the raw-packet path.)
- **Sample Rate:** 44,100 Hz
- **Channels:** 2 (Stereo)
- **Samples Per Block:** 2,048

### 4. Frame Stride Is Measured, Not Assumed
The **frame stride** (bytes per ATRAC3+ frame) depends on the encoded bitrate and
is **measured from the stream** by histogramming the spacing between successive
`0F D0 48 <xx>` sync words, rather than hard-coded. On both reference discs it is
**696 bytes**, but a disc with a different bitrate/frame size still demuxes
correctly; 696 is only the fallback for a stream too short to probe. The 4th
sync byte is a size class (`0x55` = 696, `0xD5` = 1720) and a UMD-Music disc can
switch class **mid-stream**, so the matcher must accept any class byte.

The native demuxer lives in `src/psmf_demuxer.cpp`; it scans `0xBD` PES packets,
strips the sub-header and the 8-byte frame prefix, and feeds the elementary
ATRAC3+ frames to FFmpeg's `atrac3p` decoder. On seek it re-anchors on a real
MPEG-PS pack boundary (`snap_to_pack`) and filters by PES PTS so audio and video
stay in sync.

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

### VSMX Bytecode (menu script VM)

`VSMX` is the bytecode **virtual machine** that drives UMD Video (and PSP)
interactive menus. The program is embedded in the `.RCO` container and controls
page switching, focus movement, chapter playback and controller/play-status
queries — the same machinery UMD-PG titles rely on.

- The program is a flat array of **8-byte groups** `{ id:u32, value:u32 }`. The
  **low byte of `id` is the opcode**; the remaining bytes carry small operands
  (e.g. the argument/local counts of a `FUNCTION` group).
- It is a **stack VM** (JavaScript-like): ~0x4A opcodes covering arithmetic,
  comparison, bitwise ops, constants (`CONST_INT`/`CONST_STRING`/…), properties
  and method calls, arrays, and jumps/calls.
- **Menu host objects** are registered by name (`controller`, `movieplayer`,
  `resource`, `system`, …). The machine is host-agnostic: the front-end provides
  native objects and drives the top-level script.

`include/vsmx_vm.h` / `src/vsmx_vm.cpp` provide three layers: `decode_vsmx()`
(split a raw blob into code + string tables), `disassemble()` (readable
listing), and `VsmxMachine` (a dependency-free interpreter for the subset real
menus use). On the reference disc, `JA100000.RCO` holds a **2674-instruction**
VSMX program across 5 pages; `RcoMenu` (`include/rco_menu.h`) runs it to drive
real menu navigation. See the Disc Menu section of `README.md` for the UX.

---

## Phase Roadmap

| Phase | Status | Description |
|---|---|---|
| 1 | ✅ Done | UDF ISO parser — locate & read .MPS stream sectors |
| 2 | ✅ Done | FFmpeg custom AVIO streaming + H.264 video decoding + ATRAC3+ PSMF audio reverse-engineering |
| 3 | ✅ Done | Standalone SDL2 + ImGui GUI player with synced A/V playback |
| 4 | ✅ Done | Interactive menus & chapters — `PLAYLIST.UMD` / `CLPI0088` parsing, real chapter navigation, `.RCO` object tree + VSMX menu VM |
| 4.5 | ✅ Done | Unified `DiscReader` (ISO + CHD via libchdr) — see [CHD_GUIDE.md](CHD_GUIDE.md) |
| 5 | ⏳ Planned | PPSSPP integration API |
