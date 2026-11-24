# AGENTS.md — Working on UMDPlay

Guidance for anyone (human or AI) contributing to UMDPlay. Read this before
touching the code; it records the module map, the exact build/test commands, and
the non-obvious gotchas that cost real debugging time.

See also: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) (reverse-engineered disc
internals), [`docs/TESTED_DISCS.md`](docs/TESTED_DISCS.md) (real-disc
compatibility + bug history), [`docs/CHD_GUIDE.md`](docs/CHD_GUIDE.md) (CHD
compression).

---

## What this project is

UMDPlay is a C++17 playback/preservation engine for Sony **UMD Video** discs.
It opens a UMD `.iso`/`.chd`, walks the UDF/ISO 9660 filesystem, demuxes the
MPEG-PS `.MPS` streams in memory with a native `PsmfDemuxer` reading straight
from `DiscReader`, and decodes **H.264** video + **Sony PSMF ATRAC3+** audio
through UMDPlay's own decoders — vendored `h264dec` (MIT) for video and
`src/atrac3p/` for audio, so **no FFmpeg library is linked at all**. It ships a Windows Media
Center 10-foot UI, an interactive `.RCO`/VSMX disc-menu renderer, and headless
CLI tooling (`--probe`, `--export-clip`, `--demo`).

**Design rule: nothing is hardcoded to a single disc.** Every stream list,
playlist, ATRAC3+ stride and menu is discovered at runtime. If you find yourself
adding a per-disc constant, stop and make it dynamic.

---

## Repository layout

```text
CMakeLists.txt          Build: umdplay_core, umdplay_ui_sfx, imgui_backends,
                        stb_image + umdplay_logo libs + umdplay exe + 10 test exes
cmake/EmbedAsset.cmake  Turns a binary file into a C++ byte array (build step)
include/                Public headers (one per module)
src/                    Module implementations + main.cpp (CLI/app)
tests/                  ctest suites (CHECK-macro style), fixtures/, lsan.supp
docs/                   ARCHITECTURE.md, TESTED_DISCS.md, CHD_GUIDE.md
assets/                 Brand art: umdplay_logo.png (official, embedded in the
                        binary), umdplay_flat.png, umdplay.ico + umdplay.rc (Windows)
screenshots/            README showcase images (tracked; everything else *.png ignored)
third_party/            Vendored stb_image (single header)
libs/avera-ui/          AveraUI: reusable Qt 6 Quick 10-foot shell (QML + C++)
apps/avera-player/      Qt front end: binds Decoder to AveraUI and h264dec (H.264)
.github/workflows/      ci.yml (Linux GCC + macOS arm64/x86_64: -Werror, ctest, Xvfb)
```

---

## Module map

| Module | Header / source | Responsibility |
|---|---|---|
| **iso_reader** | `include/iso_reader.h`, `src/iso_reader.cpp` | Walks the ISO 9660/UDF tree from the PVD to find `UMD_VIDEO/`/`UMD_MUSIC/` and `STREAM/`; enumerates `.MPS` streams, `RESOURCE/`, `CLIPINF/`, metadata. No fixed LBAs. |
| **disc_reader** | `include/disc_reader.h`, `src/disc_reader.cpp` | Byte-range backend chosen by extension: `.iso` (positional read) or `.chd` (libchdr + decompressed-hunk cache). Everything reads through this. |
| **psmf_demuxer** | `include/psmf_demuxer.h`, `src/psmf_demuxer.cpp` | Native, libavformat-free MPEG-2 Program Stream (`.MPS`) demuxer over `DiscReader`: parses pack/system headers + PES in one pass, emits H.264 Annex-B access units (`0xE0`) with 90 kHz PTS/DTS + keyframe flags and Sony ATRAC3+ frames (`0xBD`) with dynamic `0F D0 48 <xx>` sync + measured stride. Estimates duration, seeks by byte offset + PES-PTS filter. |
| **decoder** | `include/decoder.h`, `src/decoder.cpp` | Drives `PsmfDemuxer` → a `VideoDecoderBackend` (H.264) and an `AudioDecoderBackend` (ATRAC3+); seek/resync, A/V clock + PTS alignment, deinterlace option. Links **no** FFmpeg library. |
| **video_decoder_backend** | `include/video_decoder_backend.h`, `include/video_frame.h`, `src/h264_video_decoder.cpp` | The video seam: push Annex-B access units, pull `VideoFrame` (tight YUV420P, one allocation). Default backend wraps `third_party/h264dec` + `h264.c` (SPS geometry/PAR); `src/ffmpeg_video_decoder.cpp` is the optional A/B backend (`-DUMDPLAY_NATIVE_VIDEO=OFF`). |
| **deinterlace / yuv_convert** | `include/deinterlace.h`, `include/yuv_convert.h`, `src/deinterlace.cpp`, `src/yuv_convert.cpp` | In-memory replacements for libavfilter/yadif (vertical 1-2-1 blend of woven 480i frames) and libswscale (BT.601 YUV420P → RGB24/RGBA/BGRA, 16.16 fixed point). |
| **param_sfo** | `include/param_sfo.h`, `src/param_sfo.cpp` | Bounds-checked `PARAM.SFO` (`\0PSF`) parser — `TITLE`, `DISC_ID`, `CATEGORY`, `APP_VER`. |
| **playlist_parser** | `include/playlist_parser.h`, `src/playlist_parser.cpp` | Big-endian `PLAYLIST.UMD` (`UMDV0088`/`UMDA0088`) — tracks, clips, 90 kHz chapter markers. |
| **rco_parser** | `include/rco_parser.h`, `src/rco_parser.cpp` | Little-endian `.RCO` (`\0PRF`) container — TOC, embedded PNGs, object tree, VSMX blob + events. |
| **vsmx_vm** | `include/vsmx_vm.h`, `src/vsmx_vm.cpp` | Decode/disassemble + interpret the VSMX menu bytecode (stack VM; host objects `controller`, `movieplayer`, `resource`, `system`). |
| **rco_menu** | `include/rco_menu.h`, `src/rco_menu.cpp` | `RcoMenu` — runs a `.RCO`'s VSMX, tracks the active page + focus, dispatches button `onPush`. |
| **rco_texture** | `include/rco_texture.h`, `src/rco_texture.cpp` | Decodes `.RCO` PNG art to SDL textures for the menu renderer. |
| **wmc_ui** | `include/wmc_ui.h`, `src/wmc_ui.cpp` | The WMC 10-foot shell: hub strips, per-disc actions, player OSD, Settings overlay, Disc Menu rendering, gestures. |
| **ui_sfx** | `include/ui_sfx.h`, `src/ui_sfx.cpp` | Procedural WMC UI chimes (4 cues) + its own SDL audio device. Headless-safe. |
| **ui_manager** | `include/ui_manager.h`, `src/ui_manager.cpp` | SDL window/renderer, video texture upload, audio device + queue, demo textures. |
| **clip_export** | `include/clip_export.h`, `src/clip_export.cpp` | Headless decode → MP4 (shells out to the `ffmpeg` CLI for H.264/AAC muxing). |
| **gesture** / **aspect** / **recent** | matching headers | Touch/mouse gesture recognition, zoom/aspect modes, recent-disc persistence. |
| **atrac3p_decoder** | `include/atrac3p_decoder.h`, `src/atrac3p_decoder.cpp`, `src/atrac3p/` | Native, FFmpeg-free ATRAC3+ decoder: bitstream parser, IMDCT + windowing, gain compensation, GHA tone synthesis, power compensation and the IPQF synthesis bank. Ported from FFmpeg's atrac3plus (LGPL-2.1+, see `NOTICE`). `include/audio_decoder_backend.h` is the `AudioDecoderBackend` seam `Decoder` drives; `make_native_atrac3p_backend()` is the default implementation. |
| **avera_ui** | `libs/avera-ui/include/avera_ui/*`, `src/*`, `qml/AveraUI/*` | Reusable Qt 6 Quick shell. `AveraHub` (2-D card carousel model), `AveraPlayerController` (transport state + OSD timers over a host `AveraPlayerBackend`), `AveraVideoItem` (`VideoSurface`: YUV→RGB + `QSGSimpleTextureNode`), `AveraAudioOutput` (S16 stereo ring → `QAudioSink`), `AveraSfx` (procedural chimes). QML ships in the library's own resource at `:/avera/qml/AveraUI`. |
| **avera-player** | `apps/avera-player/main.cpp`, `Main.qml` | The Qt front end: probes the disc, fills a hub, pumps the decoder at ~60 Hz and pushes frames/PCM into the AveraUI items. |
| **logo (embedded)** | `include/umdplay_logo.h`, generated `umdplay_logo_data.cpp` | `assets/umdplay_logo.png` baked into the binary by `cmake/EmbedAsset.cmake`; drawn as the WMC hub header mark and set as the SDL window icon. |

`main.cpp` wires them together: parse args → open disc → build hub state →
event loop → dispatch `WmcAction`s.

---

## Build, test & CI

### Standard build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/bin/umdplay path/to/disc.iso
cd build && ctest --output-on-failure
```

### Strict (the CI bar) — always run before pushing

```bash
# 0-warning build: UMDPLAY_WERROR adds -Werror to every project target
cmake -B build-werror -DCMAKE_BUILD_TYPE=Release -DUMDPLAY_WERROR=ON
cmake --build build-werror -j"$(nproc)"

# Sanitizers: UMDPLAY_SANITIZE adds -fsanitize=address,undefined (+ LSan)
cmake -B build-san -DUMDPLAY_SANITIZE=ON
cmake --build build-san -j"$(nproc)"
LSAN_OPTIONS=suppressions=tests/lsan.supp \
    ctest --test-dir build-san --output-on-failure
```

`UMDPLAY_WERROR`, `UMDPLAY_SANITIZE`, `UMDPLAY_NATIVE_ATRAC3P` and
`UMDPLAY_NATIVE_VIDEO` (both default ON)
are first-class CMake options (`CMakeLists.txt`). CI also passes `-DCMAKE_CXX_FLAGS=-Werror`; both approaches
work. Vendored `libchdr`/`zstd`/`miniz` are compiled with `-w` and
`-fno-sanitize=…` so their warnings/UB never fail a project run.

### AveraUI (Qt 6 Quick front end)

Off by default — the core, CLI and tests never need Qt.

```bash
cmake -B build-qt -DCMAKE_BUILD_TYPE=Release -DUMDPLAY_AVERA_UI=ON
cmake --build build-qt -j"$(nproc)"
./build-qt/bin/avera-player disc.iso            # hub; Enter plays the feature
./build-qt/bin/avera-player disc.iso --play     # straight to the player
./build-qt/bin/avera-player disc.iso --play --screenshot shot.png --shot-delay 4000
```

To capture a demo video (no GPU needed — Xvfb + llvmpipe work):

```bash
Xvfb :99 -screen 0 1280x720x24 &
DISPLAY=:99 QT_QPA_PLATFORM=xcb ./build-qt/bin/avera-player disc.iso --play-after 3500 &
DISPLAY=:99 ffmpeg -f x11grab -framerate 30 -video_size 1280x720 -i :99 -t 18 \
    -c:v libx264 -pix_fmt yuv420p out.mp4
```

`QQuickWindow::grabWindow()` is **not** usable for frame-by-frame capture while
video is playing: each grab paints a fresh node tree, and a node created inside
the grab cannot own a texture across grabs (black frames, then a crash if you
force it). Use X11 capture, or `--screenshot` for single stills.

### Headless UI / screenshots

```bash
# SDL dummy driver + Xvfb: render the hub, player OSD or a disc menu to a PNG
SDL_AUDIODRIVER=dummy xvfb-run -a -s "-screen 0 1280x720x24" \
    ./build/bin/umdplay disc.iso --screenshot out.png
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy \
    ./build/bin/umdplay --demo --strip settings --screenshot hub.png   # no disc needed
```

### Test suites (10)

`test_psmf_demuxer` (synthetic MPEG-PS `0xE0`/`0xBD` + PTS + variable stride +
seek; optional real-ISO demux), `test_video_decoder` (native H.264 backend on a
committed 11-frame Annex-B fixture + truncated/corrupt NAL fuzzing, the
deinterlacer and the YUV converter), `test_audio_decoder` (PSMF stride detection +
optional real-ISO decode), `test_param_sfo`, `test_recent`, `test_playback`,
`test_gesture`, `test_playlist` (optional real disc via `UMD_ISO=`),
`test_chd` (committed `fixtures/pattern.chd`), `test_iso_reader` (synthetic ISO,
no disc), `test_ui_sfx`.

**Always use a local `CHECK(cond)` macro that evaluates unconditionally** — do
**not** use `assert()`, which is compiled out under `NDEBUG` (Release/CI) and
would make the test a silent no-op. See `tests/test_gesture.cpp` for the pattern.

Disc-gated tests must **skip cleanly** when no image is present (CI has none).

### LeakSanitizer suppressions

`tests/lsan.supp` suppresses only **system-library** leaks: SDL2 → dbus
(`libdbus-1`) and the Xvfb/llvmpipe GL stack (`libGLX_mesa`, `libgallium`,
`libGLX`). If you add a suppression, confirm the leak is genuinely outside
UMDPlay.

---

## Critical technical gotchas

1. **PSMF demuxing is native (`PsmfDemuxer`), not FFmpeg.** Sony omits standard
   PSMF headers on UMD Video, so FFmpeg's MPEG-PS demuxer can't be used for the
   audio (and `libavformat` is not linked at all). `src/psmf_demuxer.cpp` reads
   the `.MPS` byte range straight from `DiscReader`, walks the MPEG-PS pack
   (`00 00 01 BA`) and system (`0xBB`) headers, finds **Private Stream 1
   (`0x000001BD`)**, strips the 4-byte Sony substream header, and splits the
   concatenated payloads into ATRAC3+ frames by the `0F D0 48 <xx>` sync word —
   each frame fed to `atrac3p` as the **8-byte-prefix-stripped** body. Video
   (`0xE0`) is emitted as Annex-B access units with their 33-bit PES PTS/DTS.
   The decoder is primed with Annex-B SPS/PPS extradata (NOT an avcC record, or
   `libavcodec` would expect length-prefixed NAL units).

2. **ATRAC3+ frame sync is `0F D0 48 <xx>` — never `memcmp` 8 bytes.** The 4th
   byte is a **frame-size class**: `0x55` for the common 696-byte frame, `0xD5`
   for 1720-byte (high-bitrate) frames. Match `0F D0 48` + *any* class byte +
   four zero bytes (`is_sony_frame_sync`). UMD-Music discs switch `0x55`→`0xD5`
   **mid-stream**; hardcoding `0x55`/696 yields **0 audio bytes** on them.

2b. **One video access unit spans several PES packets.** PSMF cuts at 2048-byte
   sector boundaries, not at NAL edges, so a `0xE0` PES payload can start in the
   middle of a slice and carry no PTS. `PsmfDemuxer` concatenates the payloads and
   emits an AU only when the next **AUD** (`00 00 01 09`) shows up — feeding each
   PES as an AU makes `h264dec` treat mid-slice bytes as a NAL header (smeared,
   streaked picture). Streams that really do keep one AU per PES are detected
   from the first payload and framed the old way.

3. **Frame stride is measured, not assumed.** `PsmfDemuxer::detect_stride()`
   histograms `0F D0 48 <xx>` sync-word gaps over a byte window (dominant small
   gap wins). Frame length prefers the distance to the next sync header (clamped
   64..8192), else the detected stride. `696` is only the fallback for a
   too-short stream. The window is built from the **concatenated `0xBD`
   payloads**, not raw stream bytes, so one-frame-per-PES streams still show a
   contiguous stride.

4. **Seek correctness.** `Decoder::seek_absolute()` flushes codec buffers and
   arms a video-resync flag, then `PsmfDemuxer::seek()` repositions on a real
   MPEG-PS pack boundary (`snap_to_pack`) and arms a PES-PTS filter so packets
   before the target are dropped. The resync flag must be cleared when the
   keyframe is **sent**, not **received** — otherwise every P-frame after the
   keyframe is also dropped and playback resumes at the *next* keyframe 1–2 s
   late. Audio PES `< m_pts_anchor` is skipped, then a one-shot trim/pad removes
   residual.

5. **VSMX `END_STMT` must truncate, not clear.** Each `Frame` records a
   `stack_base` (stack depth at entry); `END_STMT` truncates to it and `RETURN`
   truncates to `fr.stack_base` before pushing. `stack.clear()` erases the
   caller's saved assignment target (e.g. `g_oPlayer = new Player()`), causing a
   stack underflow deep in a constructor. (Found on Silent Hill.)

6. **VSMX object cycles leak.** `ScriptObject` graphs can be self-referential
   (focus-out handlers leave cycles on globals); `shared_ptr` alone never frees
   them. Route **all** `make_shared<ScriptObject>` through `Impl::make_object()`,
   which records weak refs and clears props/elements/native on teardown.

7. **ISO layout is not fixed.** `UMD_VIDEO/` sits at LBA 23 *or* 24, `STREAM/`
   at 28 *or* 30, and a 90+-stream disc spills `STREAM/` across 3 sectors. Walk
   the ISO 9660 tree from the PVD (sector 16); a `0` length byte means "skip to
   the next sector". The old fixed LBAs are a legacy fallback only.

8. **`chdman createdvd` — never `createcd`.** A UMD is a 2048-byte-sector,
   single-track, DVD-style disc. `createcd` assumes 2352-byte CD sectors
   (2448-byte units) and shifts every boundary; UMDPlay then fails *cleanly*
   with "No .MPS/.PMF files found in STREAM/". Confirm `chdman info` shows
   `Unit Size: 2048`.

9. **The video path is `h264dec`, and it says what it cannot do.** UMD H.264 is
   Main profile, CABAC, 8-bit 4:2:0, frame or field pictures (PAFF) — all
   supported, bit-exact with the reference decoding process. It **rejects**
   CAVLC, MBAFF, implicit weighted prediction, memory management commands
   (MMCO) and temporal direct mode; a stream that needs them decodes up to
   that point and then returns an error (the frame is skipped, playback
   continues). UMD Video discs do not use them (the reference player decodes
   every disc it ships with), but a hand-made `x264 --interlaced` test clip
   will fail: x264 emits MBAFF, not PAFF, so it cannot be used as a fixture.

10. **`h264dec` needed two hardening patches — keep them.** ASan found a real
    heap-buffer-overflow in `h264dec_intra16x16`/`h264dec_intra_chroma`: they
    read `dst - stride` (the row above) unconditionally, which is before the
    plane when a corrupt stream selects vertical/plane prediction at the top
    row. Both now fall back to DC when the neighbour is missing. A few shifts
    of negative values became multiplications (UBSan). Details in
    `third_party/h264dec/README.md`; `test_video_decoder` fuzzes truncated and
    random NAL units to keep them honest. The vendored C is compiled with the
    sanitizers **on** (unlike libchdr) — do not add `-fno-sanitize` to it.

11. **Deinterlacing is a vertical 1-2-1 blend, not yadif.** `deinterlace_yuv420p`
    runs in place on a frame whose `interlaced` flag is set, so one frame in is
    still one frame out and the PSMF pacing is unaffected. It is a
    field-weave killer, not a motion-adaptive filter: fast motion is slightly
    soft vertically. Toggle with `Decoder::set_deinterlace()`.

12. **`--export-clip` shells out to the `ffmpeg` CLI.** Decoding can succeed
    while export fails with "ffmpeg encode failed" if `ffmpeg` isn't on PATH.

13. **Two `\x00PSF` signatures can exist** (a firmware-updater SFO and the
    UMD-Video one); the UMD-Video SFO is the **later** sector. A disc with no
    `DISC_ID`/`CATEGORY` (e.g. Blue Harvest) is normal — report `(none)`.

14. **MSYS2's `SDL2` is really `sdl2-compat` — the Windows bundle must ship
    `SDL3.dll`.** On the CI runner `pacman -S mingw-w64-x86_64-SDL2` is silently
    swapped for `mingw-w64-x86_64-sdl2-compat` (its `SDL2.dll` `LoadLibrary`s
    `SDL3.dll` at runtime), so `ldd` never lists `SDL3.dll` and a naive DLL sweep
    ships a bundle that dies with *"Failed loading SDL3 library"* /
    `0xc0000142` on a clean Windows 11 PC. `release.yml` copies `SDL3.dll`
    explicitly, then re-runs the packaged `.exe` with `/mingw64/bin` stripped
    from `PATH` to prove the bundle is self-contained.

---

## Coding style

- **C++17**, no exceptions across module boundaries; parsers are pure and
  bounds-checked, never throw, and return empty/default on malformed input.
- Warnings: project targets compile with `-Wall -Wextra -Wpedantic` (+ `-Werror`
  under `UMDPLAY_WERROR`). **`-Wconversion` is intentionally NOT enabled** — do
  not chase conversion warnings that the project doesn't ask for.
- Keep `main.cpp` orchestration-only; put logic in the modules and unit-test it.
- Match the surrounding style; keep comments for genuinely non-obvious
  invariants (like the ones above), not for restating code.
- New module → add it to `umdplay_core` (or its own lib) in `CMakeLists.txt` and
  wire the test target the same way the others are.

---

## Git & media hygiene

- **Never commit media or dumps.** `.gitignore` blocks `*.iso`, `*.chd`, `*.mp4`,
  `*.wav`, `*.bin`, `*.oma`, `*.at3`, `*.rco`, `*.mps`, `*.pmf`, `*.clp`,
  `*.vbo` and all `*.png` — **except** `screenshots/*.png`, `docs/*.png` and
  `assets/*.png`, which are the curated README images and the official brand art
  (`umdplay_logo.png` is embedded in the binary; `umdplay.ico` feeds the Windows
  `.exe`). Keep the repo root clean of stray PNGs.
- Test discs are copyrighted and fetched out-of-band; tests must skip when the
  image is absent. Do not add them to the repository.
- Build directories (`build/`, `build-*/`) and `.openhands/` are ignored.
- Branch, commit with a clear `type(scope): summary` message, and open a PR
  against `main` unless explicitly told to push to `main` directly.

### Pre-push checklist

0. `ldd build/bin/umdplay` → **no `libav*`/`libsw*`**. The default build must
   link zero FFmpeg libraries (CI asserts it); the A/B job is the only place
   libavcodec is allowed.
1. `cmake -B build-werror -DUMDPLAY_WERROR=ON && cmake --build build-werror -j"$(nproc)"` → **0 warnings**.
2. `ctest --test-dir build-werror --output-on-failure` → all pass.
3. `cmake -B build-san -DUMDPLAY_SANITIZE=ON && cmake --build build-san -j"$(nproc)"` + `LSAN_OPTIONS=suppressions=tests/lsan.supp ctest --test-dir build-san` → clean.
4. If you touched UI/decoder/parser paths: run the headless Xvfb renders and, if
   you have a disc, an `--export-clip` + `ffprobe` check.
