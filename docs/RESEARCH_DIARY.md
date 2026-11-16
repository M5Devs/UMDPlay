# UMDPlay — Reverse-Engineering & Research Diary

How **UMDPlay** went from a stubborn refusal to convert a *Hatsune Miku* UMD Video ISO into an MP4, to cracking Sony's undocumented `0xBD` ATRAC3+ framing in a Google Colab notebook, to a cross-platform C++17 player.

---

## Why This Started (June 2026)

In June 2026, I wanted to watch **Miku no Hi 39's Giving Day** (*Project DIVA* UMD Video) the way it was meant to be experienced—directly from the original disc image with its menus and chapters intact, rather than watching a compressed YouTube re-upload or transcoding it to a flat `.mp4`.

At the time, PPSSPP didn't support UMD Video playback, and Jpcsp was heavy and sluggish due to full Java-based PSP emulation. So on **June 12, 2026**, I created this repository to build a lightweight, native **C++17** UMD player from scratch.

---

## The Breakthrough Day — October 17, 2026 (Google Colab War Diary)

After building the initial UDF/ISO skeleton in July, the real reverse-engineering battle happened in a single marathon Colab session on **October 17, 2026**.

### 1. The ISO 9660 Dummy Trap (Cell 11–12)
The initial reader couldn't find any `.MPS` streams at all:
```text
[IsoReader] No .MPS/.PMF files found in STREAM/ directory
[-] Failed to parse UDF STREAM/ directory!
```
Reading the ISO 9660 Primary Volume Descriptor at sector 16 led straight into a dummy root directory (`size = 22 bytes`) that Sony uses as a compatibility stub.

To find the real filesystem, I wrote a quick Python sector scanner over `range(16, 120)`:
```python
for lba in range(16, 120):
    if b"STREAM" in sector or b"MPS" in sector:
        print(f"Match found at LBA: {lba}")
```
That uncovered the real disc map:
- **LBA 18–21:** UDF Path Tables (LE/BE)
- **LBA 24:** `UMD_VIDEO/` metadata & `PLAYLIST.UMD`
- **LBA 28:** `STREAM/` directory (`00001.MPS`, `00002.MPS`, `00098.MPS`, `00099.MPS`)

*(Later, testing 79-stream and 91-stream discs like **The Silent Hill Experience** and **Summer Days UMD-PG** showed that `UMD_VIDEO/` and `STREAM/` LBAs vary by disc and span multiple sectors, which led to replacing the initial fixed-LBA lookup with full directory-tree traversal.)*

### 2. First Video Frame (Cell 18–19)
Once `IsoReader` pointed at the UDF `STREAM/` directory and fed raw sectors into FFmpeg via a custom in-memory `AVIOContext`, the H.264 video track (`720×480 @ 29.97 fps`) decoded right away. Cell 19 rendered `miku_frame.ppm`—the first decoded frame on screen.

### 3. The `0xBD` ATRAC3+ Wall (Cell 30–41)
Video worked, but FFmpeg saw **no audio stream**:
```text
Stream #0:1: Audio -> (none)
```
Scanning the raw MPEG-PS bytes revealed `1,816` packets with start code `0x000001BD` (**Private Stream 1**):
```text
00 00 01 bd 07 ec 81 80 05 21 39 4b 81 bd 00 00 00 0c a9 71 ...
```
Sony had packed ATRAC3+ inside Private Stream 1 without standard PSMF headers. Extracting the raw `0xBD` payloads gave `3,607.91 KB` of audio data, but FFmpeg couldn't guess the `block_align`—brute-forcing standard sizes failed across the board:
```python
candidate_sizes = [192, 280, 312, 384, 480, 560, 768, 1024]
# Block Align 192 -> Error count: 85
# Block Align 280 -> Error count: 85
# Block Align 384 -> Error count: 92
# Most likely frame size: None
```

### 4. The Breakthrough: Access Unit Pointers & `0F D0 48 55` (Cell 42–44)
Inspecting the 4-byte sub-header at the start of each `0xBD` PES payload exposed a 2-byte big-endian **First Access Unit Pointer** (`payload_start + 2`):
```python
frame_ptr = struct.unpack(">H", data[payload_start+2 : payload_start+4])[0]
real_frame_start = payload_start + 4 + frame_ptr
```
Printing the bytes at `real_frame_start` across consecutive packets revealed the exact undocumented Sony frame sync header:
```text
Packet #1 | Pointer: +12  | Frame Start: 0f d0 48 55 00 00 00 00
Packet #2 | Pointer: +84  | Frame Start: 0f d0 48 55 00 00 00 00
Packet #3 | Pointer: +159 | Frame Start: 0f d0 48 55 00 00 00 00
```
Measuring the distance between consecutive `0F D0 48 55 00 00 00 00` headers gave an exact, constant stride:
```text
Frame distances: [696, 696, 696, 696, 696, 696, 696, 696, 696]
Full Sony Frame Length : 696 bytes
8-byte Sony Sync Header: 0F D0 48 55 00 00 00 00
Pure ATRAC3+ Payload   : 688 bytes (the exact block_align!)
```
Stripping the 8-byte header across **5,253 extracted frames** and feeding the 688-byte payloads into FFmpeg's `atrac3p` decoder produced crystal-clear stereo audio at 44,100 Hz. Miku finally sang straight out of the `.iso`.

---

## Generalizing Beyond One Disc (November 2026)

With the core format cracked, testing additional discs exposed four more real-world edge cases that shaped the final C++17 engine:

1. **Post-Seek A/V Lip-Sync & 480i Deinterlacing (*Family Guy: Blue Harvest*):**
   - Exposed a ~1–2 s post-seek audio lead and horizontal combing on 480i NTSC action scenes. Fixed by aligning manual `0xBD` audio PTS to the video keyframe PTS and adding `yadif` deinterlacing.
2. **Variable ATRAC3+ Frame Syncs (*Girl Next Door — UMD Music*):**
   - Its lead-in used `0F D0 48 55` (696 B), then switched mid-stream to `0F D0 48 D5` (**1,720 B** high-bitrate ATRAC3+). Replaced the fixed sync check with a generic `0F D0 48 <size_class> 00 00 00 00` matcher and dynamic stride measurement.
3. **Complex `VSMX` Bytecode & Stack Frames (*The Silent Hill Experience*):**
   - Its 21,637-instruction `.RCO` menu script exposed a `VSMX_END_STMT` stack-wipe bug inside nested `new Player()` constructors (fixed via per-frame `stack_base` tracking) and a `shared_ptr` cycle leak on teardown.
4. **Multi-Sector Directories (*Summer Days UMD-PG* — 91 streams):**
   - Replaced fixed LBA 24/28 reads with full directory-tree walking across multi-sector ISO directories.

---

*Every line of the C++17 demuxer, dynamic stride detector, and VSMX interpreter in `UMDPlay` grew directly out of these real-disc findings.*
