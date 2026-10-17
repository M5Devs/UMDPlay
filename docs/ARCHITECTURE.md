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

## Phase Roadmap

| Phase | Status | Description |
|---|---|---|
| 1 | ✅ Done | UDF ISO parser — locate & read .MPS stream sectors |
| 2 | ✅ Done | FFmpeg custom AVIO streaming + H.264 video decoding + ATRAC3+ PSMF audio reverse-engineering |
| 3 | 🔄 Next | Standalone SDL2 + ImGui GUI player with synced A/V playback |
| 4 | ⏳ Planned | Interactive menu system (.RCO / .VBO reverse-engineering) |
| 5 | ⏳ Planned | PPSSPP integration API |
