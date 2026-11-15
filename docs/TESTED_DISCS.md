# UMDPlay — Tested Disc Compatibility Report

This document records every **real-world UMD disc** UMDPlay has been tested
against, what each disc exercised, and the concrete bugs it exposed and fixed.
Everything below was measured on the actual disc images; nothing here is
theoretical.

The goal is simple: UMDPlay must handle **any** UMD Video disc with **no
per-disc code paths**. Nothing is hardcoded to a single title — each disc's
streams, playlist, ATRAC3+ frame stride, menu resources and geometry are
discovered at runtime.

> Reproduce any row with `./build/bin/umdplay <disc.iso> --probe` (structure) or
> `--export-clip clip.mp4 --start <seconds> --clip-seconds 12` (A/V decode).

---

## Summary matrix

| # | Disc | Region / type | Size | Streams | Menu (`.RCO`) | ATRAC3+ stride |
|---|---|---|---|---|---|---|
| 1 | Miku no Hi 39's Giving Day | JP · UMD-Video (concert) | 1.73 GB | 4 | `JA100000.RCO`, 81 images / 5 pages | 696 B (`0F D0 48 55`) |
| 2 | Family Guy: Blue Harvest | US · UMD-Video (film) | 545 MB | 3 | — (no interactive menu) | 696 B (`0F D0 48 55`) |
| 3 | Girl Next Door — Be your wings | JP · UMD-Music (music video) | 288 MB | 1 | — | **1720 B** (`0F D0 48 D5`) |
| 4 | The Silent Hill Experience | EU · UMD-Video (digital comic) | 1.76 GB | 34 | `FR000001.RCO`, 24 pages / 148 objects | 696 B |
| 5 | CHD round-trip | — (container test) | — | — | — | — |

All five pass: `--probe` parses the filesystem, the main `.MPS` demuxes, and a
12 s mid-movie `--export-clip` decodes to clean H.264 720×480 + AAC 44.1 kHz
stereo with zero decode errors.

---

## 1. Miku no Hi 39's Giving Day — JP UMD-Video

The original bring-up disc and the reference for the whole project.

| Property | Value |
|---|---|
| TITLE (`PARAM.SFO`) | ミクの日感謝祭 39's Giving Day |
| DISC_ID / CATEGORY | *(absent — the SFO carries only `TITLE`)* |
| Size | 1,734,440,960 B (1.73 GB) |
| MD5 | `1d9cc5553b5f2d8ad6e3d3882a58592c` |
| Streams | 4 — `00001.MPS` **main, 1648 MB, 118:25**; `00002.MPS` 4 MB; `00098`/`00099` near-empty |
| Playlist | `PLAYLIST.UMD` magic `UMDV0088` (v88), 4 tracks |
| Main track | `3_Honpen` → clip `00001`, **45 chapters** |
| Menu | `RESOURCE/JA100000.RCO` — 180,848 B, `\0PRF` v0x80, **81 PNG images**, 51 objects, **5 pages**, VSMX `main` = **2,674 instructions** |
| Pages | `main_menu`, `chapter1_menu`, `chapter2_menu`, `chapter3_menu`, `black` |
| Cover art | `ICON0.PNG` present; `PIC1.PNG` / `SND0.AT3` absent |
| ATRAC3+ | `0F D0 48 55`, **696-byte** stride, 44.1 kHz stereo |

**Why it matters.** This disc established the entire reverse-engineering chain:
the UDF/ISO 9660 overlay, the dummy ISO 9660 PVD (root dir `size=22`, unusable),
the `.MPS` MPEG-PS layout, the hand `0xBD` PSMF ATRAC3+ demux, `PLAYLIST.UMD`
chapter markers, and the `.RCO`/VSMX menu model. It is also the first disc that
proved the **interactive Disc Menu** works end-to-end (all 5 pages, both
`chapter*_menu` pages navigate).

**Verified clip.** `--export-clip` at `--start 3552` → `test_clip.mp4`,
12.012 s, H.264 High 720×480 @ 29.97 fps + AAC 44.1 kHz stereo, video frames
non-black (YAVG 20–30, distinct), audio mean −12.2 dB / peak −0.2 dB, full
`ffmpeg -f null` re-decode = **0 errors**.

---

## 2. Family Guy: Blue Harvest — US Region-1 UMD-Video

A second, deliberately *different* disc to prove nothing is Miku-specific.

| Property | Value |
|---|---|
| TITLE | Family Guy: Blue Harvest |
| Size | 570,720,256 B (545 MB) |
| Streams | 3 — `00001.MPS` **main, 533 MB**; `00002.MPS` 7 MB; `00003.MPS` 1 MB |
| Cover art | `ICON0.PNG` **and** `PIC1.PNG` present; `SND0.AT3` present |
| ATRAC3+ | `0F D0 48 55`, **696-byte** stride (same as Miku) |

**What it exercised**

- **480i NTSC deinterlacing.** Every frame on both discs carries
  `AV_FRAME_FLAG_INTERLACED` (flags `0x8`/`0xA`). UMDPlay deinterlaces with
  `yadif` via libavfilter (an **optional** dependency — without it, frames pass
  through untouched and the build still works). The buffersrc args must pass
  `range=`/`colorspace=` or FFmpeg spams *"Changing video frame properties on
  the fly is not supported"*.
- **SFO without `DISC_ID` / `CATEGORY`.** Its own `PARAM.SFO` (sector 48)
  carries only `TITLE` + `PARENTAL_LEVEL`, so `DISC_ID: (none)` and
  `CATEGORY: (none)` are **correct**, not a bug. (Note: sector 32 holds the
  *PSP firmware updater's* SFO — a different payload. When two `\x00PSF`
  signatures exist, the UMD-Video one is the later sector.)
- **`--probe` metadata fix.** `print_disc_summary` reported ICON0/PIC1 as
  "absent" because it queried SDL textures that are only created in windowed
  mode. It now reports on-disc presence from the metadata list
  (`DiscItem::has_icon/has_pic1/has_snd0`), so `--probe` is accurate headless.
- **Post-seek A/V PTS alignment.** Seeking to the middle of this disc exposed a
  video resync bug (see §6) where playback resumed 1–2 s late; the fix drops
  stale audio and clears the resync flag on the *keyframe send*, not the
  keyframe *receive*.
- **Real content ends ~2870 s**, even though the container advertises 7103 s —
  seeking past the true end decodes 0 frames. UMDPlay handles that gracefully.

**Verified clip.** `--export-clip --start 1200` → 12.012 s, H.264 720×480 +
AAC 44.1 kHz stereo, audio mean −17.4 dB / peak −5.4 dB, 0 decode errors.

---

## 3. Girl Next Door — Be your wings — JP UMD-Music

The disc that broke the audio demuxer, and the reason the ATRAC3+ sync is now
class-byte agnostic.

| Property | Value |
|---|---|
| Type | UMD-Music / Music Video (UMD-Audio family) |
| Size | ~288 MB |
| ATRAC3+ | lead-in `0F D0 48 55` (**696 B**), body `0F D0 48 D5` (**1720 B**) |

**Root cause of "0 audio bytes".** The hand PSMF demuxer matched the 8-byte
Sony frame sync `0F D0 48 55` with `memcmp(..., 8)`. UMD-Music discs share the
`0F D0 48` prefix but **the frame-size class byte varies**: lead-in frames are
`0F D0 48 55` (696-byte stride), the body switches to `0F D0 48 D5`
(**1720-byte** high-bitrate stride). The strict `memcmp` never matched the body,
so `process_psmf_audio()` found no frames and produced 0 PCM. UMD-Video discs
are uniformly `0x55`/696, which is why they were unaffected.

**Fix (dynamic sync + stride detection).**

- `is_sony_frame_sync(p, avail)` matches `0F D0 48`, **any** class byte, then
  four zero bytes (`p[4..7] == 0`) — replacing the rigid `memcmp` in the probe,
  `detect_stride_from()`, `process_psmf_audio()` and the
  `process_audio_packet()` fallback.
- Frame length is taken from the distance to the **next** sync header when one
  is visible (clamped 64..8192), otherwise the detected stride — so the
  mid-stream 696 → 1720 transition, and variable-bitrate discs, both demux.
- `detect_stride_from()` histograms `0F D0 48 <xx>` sync-word gaps over a
  512 KB probe window (the dominant *small* gap wins; large gaps from
  interleaved video never do). `696` is only the fallback for a stream too short
  to probe.

**Regression check.** UMD-Video PCM is **byte-identical** before/after
(md5 `8afc1048c72423cd139a56d39b23e2d0`, 240 frames @ 900 s) — the fix
introduced zero change for discs that already worked.

**Verified.** A 40 s music clip now decodes clean, uniform RMS ≈ 11 k, no
artifacts, no decoder errors.

---

## 4. The Silent Hill Experience (Europe) — Interactive UMD-Video

A digital-comic disc with the largest and most complex menu tested, and the
disc that stress-tested both the VSMX VM and the ISO reader.

| Property | Value |
|---|---|
| Type | Interactive UMD-Video / Digital Comic |
| Size | 1,762,326,528 B (1.76 GB) |
| Streams | **34** `.MPS` — main `00019.MPS` (39 MB, "Digital Comic", 11 chapters) |
| Playlist | `PLAYLIST.UMD` magic `UMDV0088`, **79 tracks** |
| Menu | `RESOURCE/FR000001.RCO` — `\0PRF` v0x71, **148 objects, 24 pages**, 32 PNG textures, VSMX `main` = **21,637 instructions** |
| Layout | `UMD_VIDEO/` at LBA **23** (not 24), `STREAM/` off the fixed LBA |

**Bug 1 — `VSMX_END_STMT` wiped the caller's stack.** The VM died at
instruction 4096 (`g_oPlayer = new Player()`): a constructor like
`this.x = x;` runs each statement's `END_STMT`, which did `stack.clear()` —
**also erasing the caller's saved assignment target** (the `g_oPlayer` reference
pushed *before* `CALL_NEW`). `ASSIGN` then saw an empty stack and raised
"stack underflow" after 959 steps, so `onAutoPlay` was never dispatched.

*Fix:* each `Frame` records a `stack_base` (the stack depth at entry);
`END_STMT` **truncates to `stack_base`** instead of clearing, and `RETURN` also
truncates to `fr.stack_base` before pushing its result. Silent Hill's VSMX now
runs to completion (4959 steps, no error) and `onAutoPlay` reaches the host.

**Bug 2 — fixed-LBA ISO reader silently dropped streams.** `src/iso_reader.cpp`
hardcoded `UMD_VIDEO/` at LBA 24 and `STREAM/` at LBA 28, reading a single
2048-byte sector. Real discs vary: `UMD_VIDEO/` at 23 (Silent Hill) or 24
(Miku); `STREAM/` at 28 or 30; a 90+-stream disc spills `STREAM/` across 3
sectors. *Fix:* walk the ISO 9660 tree from the PVD root directory record
(sector 16) to find the content root (`UMD_VIDEO/` / `UMD_MUSIC/`) and
`STREAM/`, following multi-sector directories (a `0` length byte skips to the
next sector). The old fixed LBAs remain only as a legacy fallback. **Silent
Hill went from 34 → 79 discovered streams** (45 were being silently dropped).

**Menu-model gap (not a crash).** Silent Hill builds all button geometry at
runtime (`MPSize.setSize()`), so RCO objects carry `w = h = 0` and `pageID` /
`focusID` are `nan`. `RcoMenu::rebuild_page` used to drop zero-geometry items,
leaving the page with 0 buttons; it now falls back to the PNG's pixel size.

**Verified.** `--export-clip` from `00019.MPS` → 12.012 s H.264 720×480 +
ATRAC3+→AAC 44.1 kHz stereo, mean −13.7 dB (non-silent); menu opens and renders
(`--open-menu`); VSMX runs to completion; ASan/UBSan clean.

---

## 5. CHD round-trip (`chdman createdvd` vs `createcd`)

Not a disc, but a first-class compatibility guarantee: a UMD ISO compressed to
CHD must round-trip **byte-identically** and decode **identically**.

| Test | Result |
|---|---|
| `chdman createdvd -i movie.iso -o movie.chd` | ✅ Unit Size **2048**, `DVD ` metadata |
| `chdman verify -i movie.chd` | ✅ Raw + Overall SHA-1 verification successful |
| `chdman extractdvd` → `cmp original.iso restored.iso` | ✅ byte-identical |
| UMDPlay ISO vs CHD twin `--export-clip` | ✅ **byte-identical** MP4 clips |
| `chdman createcd` (2352-byte CD mode) | ❌ Unit Size 2448 → `--probe` fails cleanly |

A UMD is a **2048-byte-sector, single-track, DVD-style** disc, so it must be
compressed with `createdvd`. `createcd` assumes 2352-byte CD sectors (2448-byte
units), shifting every sector boundary; UMDPlay's UDF parser then lands in the
wrong place and reports *"No .MPS/.PMF files found in STREAM/ directory"* — a
**clean** failure, not silent corruption. Full details:
[`docs/CHD_GUIDE.md`](CHD_GUIDE.md).

---

## 6. Cross-cutting fixes proven by these discs

| Fix | Discovered on | Summary |
|---|---|---|
| Dynamic ATRAC3+ stride | Girl Next Door | Histogram `0F D0 48 <xx>` gaps instead of assuming 696 B |
| Class-byte-agnostic sync | Girl Next Door | Match `0F D0 48` + any class byte + four zero bytes |
| `VSMX_END_STMT` `stack_base` | Silent Hill | Truncate to frame base instead of `stack.clear()` |
| VSMX object-cycle leak | Silent Hill | Track `ScriptObject`s and break cycles on teardown |
| Dynamic ISO tree walk | Silent Hill / UMD-PG | Find `UMD_VIDEO/` + `STREAM/` from the PVD, not fixed LBAs |
| Keyframe-accurate seek | Blue Harvest | Drop to the next `AV_PKT_FLAG_KEY`; 152 corrupt-macroblock errors → **0** |
| Post-seek A/V PTS align | Blue Harvest | Parse PES PTS, skip audio `< anchor`, trim residual; lead 2.1 s → 3–67 ms |
| 480i `yadif` deinterlace | both video discs | Optional libavfilter; pass `range=`/`colorspace=` |
| `--probe` cover-art accuracy | Blue Harvest | Report on-disc presence, not windowed-only SDL textures |
| Recent-disc dedup | — | `weakly_canonical()` identity key (resolves symlinks / `..`) |

---

## Adding a new disc

1. `./build/bin/umdplay new_disc.iso --probe` — confirm the filesystem parses
   and the stream list looks right.
2. `./build/bin/umdplay new_disc.iso --probe --verbose` — check the RCO pages
   and VSMX instruction count if the disc ships a menu.
3. `./build/bin/umdplay new_disc.iso --export-clip clip.mp4 --start <mid> --clip-seconds 12`
   then `ffprobe clip.mp4` and a full `ffmpeg -f null` re-decode for errors.
4. If it is an interactive disc: `./build/bin/umdplay new_disc.iso --open-menu`
   and step through the pages.

If a disc fails, the failure is almost always one of: a fixed LBA the tree walk
should replace, a new ATRAC3+ class byte, or a VSMX opcode the interpreter does
not yet implement.
