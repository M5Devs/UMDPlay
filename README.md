# UMDPlay (UMD Video Player)

![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20Android-lightgrey)
![Status](https://img.shields.io/badge/Status-Planning-yellow)
![Language](https://img.shields.io/badge/Language-C%2B%2B17-orange)
![Build System](https://img.shields.io/badge/Build-CMake-red)
![PRs Welcome](https://img.shields.io/badge/PRs-Welcome-brightgreen)

> A lightweight, cross-platform C++ library and standalone media player designed to parse, decode, and play **UMD Video ISOs** — the missing link in PSP preservation.

The ultimate goal of this project is to achieve a stable, low-overhead playback layer for UMD Video content that can be seamlessly integrated into existing PSP emulators (such as **PPSSPP**) or used as an independent preservation tool.

---

## 🚀 Vision & Motivation

While PlayStation Portable (PSP) game emulation has reached near-perfection, native support for **UMD Video ISOs** remains largely unsupported or unstable across all major emulators.

UMD Video discs use a specific filesystem structure rooted in the `UMD_VIDEO/` directory, containing:
- **MPEG Program Stream (`.MPS`)** containers with **H.264 AVC or MPEG-2** video tracks (title-dependent)
- **ATRAC3plus**, **AC3**, or **LPCM** audio formats
- Interactive menu overlays built on **`.RCO` / `.VBO`** assets

**UMDPlay** aims to bridge this gap by building a dedicated, efficient decoder from the ground up — in pure C++ with FFmpeg — heavily optimized to run even on low-spec legacy hardware.

---

## 🛠️ Tech Stack & Dependencies

| Component | Technology | Notes |
|---|---|---|
| Language | C++17 | Modern standard, no C++20 for max compiler compatibility |
| Decoder | FFmpeg (libavcodec / libavformat) | Linked as pre-compiled static libraries |
| Windowing & Events | SDL2 | Software renderer default; optional GPU backend for capable systems |
| Build System | CMake | Cross-platform, supports MinGW / MSVC / GCC / NDK |

### 🎯 Target Platforms

| Platform | Toolchain | Status |
|---|---|---|
| 🪟 Windows | MinGW / MSVC | Primary target |
| 🐧 Linux | Native GCC | Primary target |
| 🤖 Android | Android NDK | Long-term — contributions welcome |

---

## 📂 Project Structure

```text
UMDPlay/
├── CMakeLists.txt              # Core build configuration
├── docs/
│   └── ARCHITECTURE.md         # Technical breakdown of the decoding pipeline
├── include/                    # Public header files
│   ├── iso_reader.h            # ISO 9660/UDF filesystem + UMD_VIDEO/ parser
│   ├── decoder.h               # FFmpeg context handles, demuxer, A/V sync
│   └── ui_manager.h            # SDL2 rendering loop and event handling
├── src/                        # Implementation files
│   ├── main.cpp                # Application entry point
│   ├── iso_reader.cpp
│   ├── decoder.cpp
│   └── ui_manager.cpp
└── thirdparty/                 # Vendored static libraries + headers
    ├── ffmpeg/
    └── sdl2/
```

---

## 🗺️ Roadmap

- [ ] **Phase 1** — ISO Parser: navigate `UMD_VIDEO/STREAM/`, extract raw `.MPS` bytes from the UDF filesystem
- [ ] **Phase 2** — FFmpeg Integration: demux and decode `.MPS` containers with strict A/V synchronization
- [ ] **Phase 3** — SDL2 GUI: standalone player with Play, Pause, Seek, and Volume controls
- [ ] **Phase 4** — Menu Research: reverse-engineer UMD interactive menus via `.RCO` / `.VBO` overlays
- [ ] **Phase 5** — PPSSPP API: expose a clean C-style API for upstream integration

---

## 🔧 Building from Source

> ⚠️ Build instructions will be available once Phase 1 is complete.  
> For dependency notes and architecture decisions, see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

---

## 🖼️ Verified Decoded Frames (Proof of Concept)

| Hatsune Miku: Project DIVA (39's Giving Day) | Family Guy: Blue Harvest (Star Wars Parody) |
|:---:|:---:|
| ![Miku](screenshots/miku1.png) | ![Family Guy](screenshots/fg1.png) |
| *Japanese UMD Video (Concert)* | *US Commercial UMD Video (Hollywood)* |

---

## 🤝 Contributing

This project is open to **Reverse Engineers**, **Emulation Developers**, and **Multimedia enthusiasts**.

Before submitting a pull request or opening a structural issue, please review the architecture notes in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) to align with the project's design philosophy.

For smaller contributions, feel free to open an issue and start a discussion.

---

## 📜 License

This project is licensed under the **GNU General Public License v2.0 (GPLv2)**.

GPLv2 was chosen specifically to ensure **license compatibility with the PPSSPP project**, enabling clean upstream integration without legal friction.

*Disclaimer: This project is strictly for digital preservation and educational purposes. "PSP" and "UMD" are trademarks of Sony Interactive Entertainment Inc.*
