# UMDVPlay — Architecture Notes

## UMD Video ISO Layout (Verified)

UMD Video discs use **UDF 1.02**, not plain ISO 9660.
The ISO 9660 PVD at sector 16 is a **dummy** — its root directory has size=22 bytes
and cannot be used for filesystem traversal.

### Real UDF Layout

```
Sector 16     : ISO 9660 PVD       (dummy — root dir size=22, ignore)
Sectors 18-19 : UDF Path Table LE  (UMD_VIDEO @ LBA 24)
Sectors 20-21 : UDF Path Table BE  (UMD_VIDEO @ LBA 24, big-endian)
Sector  22    : UDF Volume Set Descriptor
Sector  24    : UMD_VIDEO/  ICB (File Entry)
Sector  28    : STREAM/ File Identifier Descriptors  ← .MPS entries here
LBA  288      : 00098.MPS  (00 00 01 BA — MPEG Program Stream ✅)
LBA  592      : 00002.MPS  (00 00 01 BA ✅)
LBA 2880      : 00001.MPS  (00 00 01 BA ✅)
```

Verified against: *Miku no Hi 39's Giving Day — Project DIVA [UMD VIDEO]*

---

## UDF File Identifier Descriptor (simplified)

Each record in the STREAM/ sector:

```
Offset  Size  Field
+0x00   2     record_length (uint16 LE)
+0x02   2     LBA of file data (uint16 LE)
+0x04   4     partition reference / flags
+0x08   1     filename_length
+0x09   N     filename (ASCII on UMD Video discs)
```

Total record size = 0x38 bytes for standard .MPS entries.

---

## MPS File Format

`.MPS` files are **MPEG Program Stream** containers (ISO 13818-1).

Magic bytes: `00 00 01 BA` (Pack Start Code)

Contents:
- Video: H.264 AVC (stream ID `0xE0`) or MPEG-2 Video (title-dependent)
- Audio: ATRAC3plus (private stream `0xBD`) or LPCM

FFmpeg can demux and decode `.MPS` directly via `libavformat` / `libavcodec`.

---

## Decoding Pipeline (Phase 2)

```
ISO file
  └─ iso_reader → raw .MPS bytes (or direct offset seek)
       └─ AVFormatContext (FFmpeg) → demux streams
            ├─ AVCodecContext H.264  → raw YUV frames
            └─ AVCodecContext ATRAC3+ → PCM audio samples
                 └─ SDL2 AudioDevice → playback
```

---

## Phase Roadmap

| Phase | Status | Description |
|---|---|---|
| 1 | ✅ Done | UDF ISO parser — locate & read .MPS files |
| 2 | 🔄 Next | FFmpeg demux + decode, raw frame output |
| 3 | ⏳ Planned | SDL2 + ImGui GUI player |
| 4 | ⏳ Planned | UMD menu system (.RCO / .VBO) |
| 5 | ⏳ Planned | C API for PPSSPP integration |
