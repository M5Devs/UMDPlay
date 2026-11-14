#include "iso_reader.h"
#include "decoder.h"
#include "ui_manager.h"
#include "wmc_ui.h"
#include "clip_export.h"

#include "stb_image_write.h"   // declarations only (impl is in stb_impl.cpp)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class Mode { Hub, Player };

struct Options {
    std::string iso_path;
    std::string screenshot_path;   // --screenshot <png>: render, write a PNG, exit
    std::string export_path;       // --export-clip <mp4>: encode a clip and exit
    double      start_seconds = 0.0;
    double      clip_seconds  = 12.0;
    bool        probe_only    = false;
    bool        autoplay      = false;   // --play: skip the Hub, start the main stream
    bool        demo          = false;   // --demo: render a synthetic hub, no ISO
};

void print_usage() {
    std::cout <<
        "Usage: umdplay <path_to_umd_video.iso> [options]\n"
        "  --play                 start the main feature immediately\n"
        "  --export-clip <mp4>    decode a clip to MP4 (H.264/AAC) and exit\n"
        "  --clip-seconds <n>     clip length in seconds (default 12)\n"
        "  --screenshot <png>     render a frame, write a PNG, then exit\n"
        "  --start <seconds>      start at this offset\n"
        "  --probe                print stream/metadata info and exit\n"
        "  --demo                 render the hub with synthetic data (no ISO)\n";
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--screenshot" && i + 1 < argc) {
            opt.screenshot_path = argv[++i];
        } else if (a == "--export-clip" && i + 1 < argc) {
            opt.export_path = argv[++i];
        } else if (a == "--clip-seconds" && i + 1 < argc) {
            opt.clip_seconds = std::atof(argv[++i]);
        } else if (a == "--start" && i + 1 < argc) {
            opt.start_seconds = std::atof(argv[++i]);
        } else if (a == "--probe") {
            opt.probe_only = true;
        } else if (a == "--play") {
            opt.autoplay = true;
        } else if (a == "--demo") {
            opt.demo = true;
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return false;
        } else if (!a.empty() && a[0] != '-' && opt.iso_path.empty()) {
            opt.iso_path = a;
        }
    }
    return true;
}

void print_disc_summary(const DiscItem& d) {
    std::cout << "[*] Disc: " << (d.title.empty() ? "(untitled)" : d.title) << "\n";
    std::cout << "    DISC_ID : " << (d.disc_id.empty()  ? "(none)" : d.disc_id)  << "\n";
    std::cout << "    CATEGORY: " << (d.category.empty() ? "(none)" : d.category) << "\n";
    std::cout << "    ICON0   : " << (d.icon ? "loaded" : "absent") << "\n";
    std::cout << "    PIC1    : " << (d.pic1 ? "loaded" : "absent") << "\n";
    std::cout << "    Streams : " << d.streams.size() << "\n";
    for (const auto& s : d.streams) {
        std::cout << "      - " << (s.is_main ? "[MAIN] " : "       ") << s.name
                  << "  " << (s.size_bytes / (1024u * 1024u)) << " MB\n";
    }
}

// Translate a gamepad button into a synthetic SDL key event so ImGui's nav
// layer picks it up exactly like a physical keypress.
void push_key(SDL_Keycode sym, bool down) {
    SDL_Event e;
    SDL_zero(e);
    e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    e.key.type = e.type;
    e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    e.key.keysym.sym = sym;
    e.key.keysym.scancode = SDL_GetScancodeFromKey(sym);
    SDL_PushEvent(&e);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        return 1;
    }
    if (opt.iso_path.empty() && !opt.demo) {
        print_usage();
        return 1;
    }

    std::cout << "=== UMDPlay Preservation Engine (Phase 3 - WMC UI) ===\n";

    DiscItem disc;
    std::vector<MpsEntry> entries;
    std::vector<uint8_t> icon_png, pic1_png;
    bool has_iso = false;

    // --demo builds a synthetic disc so the UI can be exercised without an ISO
    // (keeps CI free of copyrighted assets).
    if (opt.demo) {
        disc.title    = "\u30c7\u30e2 UMD Video (Demo)";
        disc.disc_id  = "UMDPLAY-DEMO";
        disc.category = "UMD_VIDEO";
        const char* names[] = {"00001.MPS", "00002.MPS", "00003.MPS"};
        const uint32_t sizes[] = {1500000000u, 4000000u, 1200000u};
        for (int i = 0; i < 3; ++i) {
            StreamItem s;
            s.name = names[i];
            s.size_bytes = sizes[i];
            s.is_main = (i == 0);
            disc.streams.push_back(s);
        }
    } else {
        std::cout << "[*] Inspecting ISO: " << opt.iso_path << "\n";

        IsoReader reader(opt.iso_path);
        if (!reader.open()) {
            std::cerr << "[-] Failed to read UMD Video filesystem!\n";
            return 1;
        }

        entries = reader.get_mps_entries();
        if (entries.empty()) {
            std::cerr << "[-] No .MPS streams found!\n";
            return 1;
        }

        disc.iso_path = opt.iso_path;
        disc.title    = reader.param().title;
        disc.disc_id  = reader.param().disc_id;
        disc.category = reader.param().category;

        uint32_t max_size = 0;
        for (const auto& e : entries) max_size = std::max(max_size, e.size_bytes);
        for (const auto& e : entries) {
            StreamItem s;
            s.name       = e.name;
            s.size_bytes = e.size_bytes;
            s.is_main    = (e.size_bytes == max_size);
            disc.streams.push_back(s);
        }
        if (reader.has_metadata("ICON0.PNG")) icon_png = reader.read_metadata("ICON0.PNG");
        if (reader.has_metadata("PIC1.PNG"))  pic1_png = reader.read_metadata("PIC1.PNG");
        has_iso = true;
    }

    print_disc_summary(disc);

    // Headless clip export: no window needed, encode straight from the decoder.
    if (!opt.export_path.empty()) {
        if (!has_iso || entries.empty()) {
            std::cerr << "[-] --export-clip needs a real ISO (not --demo).\n";
            return 1;
        }
        MpsEntry main_entry = entries[0];
        for (const auto& e : entries) {
            if (e.size_bytes > main_entry.size_bytes) main_entry = e;
        }
        std::cout << "[*] Exporting " << opt.clip_seconds << "s clip from "
                  << main_entry.name << " starting at " << opt.start_seconds << "s\n";
        const bool ok = export_clip(opt.iso_path, main_entry, opt.start_seconds,
                                    opt.clip_seconds, opt.export_path);
        std::cout << (ok ? "[+] Clip written: " : "[-] Clip export failed: ")
                  << opt.export_path << "\n";
        return ok ? 0 : 1;
    }

    UiManager ui;
    if (!ui.init("UMDPlay - Windows Media Center UI", 1280, 720)) {
        std::cerr << "[-] Failed to initialize SDL window/renderer!\n";
        return 1;
    }

    // Decode ICON0.PNG / PIC1.PNG into SDL textures via a temporary WMC context.
    if (!icon_png.empty() || !pic1_png.empty()) {
        WmcUi tex;
        if (tex.init(ui.window(), ui.renderer(), disc.title)) {
            if (!icon_png.empty()) disc.icon = tex.texture_from_png(icon_png);
            if (!pic1_png.empty()) disc.pic1 = tex.texture_from_png(pic1_png);
            tex.close();
        }
    }

    if (opt.probe_only) {
        ui.close();
        return 0;
    }

    WmcUi wmc;
    if (!wmc.init(ui.window(), ui.renderer(), disc.title)) {
        std::cerr << "[-] Failed to initialize WMC UI (ImGui)!\n";
        ui.close();
        return 1;
    }

    std::vector<DiscItem> discs = { disc };
    int selected_disc = 0;
    int selected_stream = 0;
    for (size_t i = 0; i < disc.streams.size(); ++i) {
        if (disc.streams[i].is_main) { selected_stream = static_cast<int>(i); break; }
    }

    Mode mode = Mode::Hub;
    Decoder decoder;
    AVFrame* video_frame = av_frame_alloc();
    std::vector<uint8_t> audio_pcm;
    bool paused = false;
    bool osd_visible = true;
    double elapsed = 0.0, total = 0.0;
    double fps = 29.97;
    uint32_t frame_delay_ms = 33;
    uint32_t last_frame_time = 0;
    bool decoder_open = false;
    bool quit = false;
    int shot_countdown = -1;

    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    }

    const float t0 = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    auto now = [&]() { return static_cast<float>(SDL_GetTicks()) / 1000.0f - t0; };

    auto close_decoder = [&]() {
        if (decoder_open) { decoder.close(); decoder_open = false; }
    };

    auto open_stream = [&](const StreamItem& s) -> bool {
        MpsEntry e;
        e.name = s.name;
        e.size_bytes = s.size_bytes;
        e.lba = 0;
        for (const auto& m : entries) {
            if (m.name == s.name) { e.lba = m.lba; break; }
        }
        close_decoder();
        if (!has_iso || !decoder.open_stream(opt.iso_path, e)) return false;
        decoder_open = true;
        fps = decoder.get_fps();
        if (fps <= 0.0 || fps > 120.0) fps = 29.97;
        frame_delay_ms = static_cast<uint32_t>(1000.0 / fps);
        total = decoder.get_duration();
        elapsed = 0.0;
        if (opt.start_seconds > 0.0) decoder.seek_relative(opt.start_seconds);
        last_frame_time = SDL_GetTicks();
        return true;
    };

    if (opt.autoplay && !disc.streams.empty()) {
        if (open_stream(disc.streams[selected_stream])) {
            mode = Mode::Player;
            std::cout << "[*] Autoplay: " << disc.streams[selected_stream].name << "\n";
        }
    }

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            wmc.process_event(ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q) quit = true;
            if (ev.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (ev.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  push_key(SDLK_LEFT, true); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: push_key(SDLK_RIGHT, true); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_UP:    push_key(SDLK_UP, true); break;
                    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  push_key(SDLK_DOWN, true); break;
                    case SDL_CONTROLLER_BUTTON_A:          push_key(SDLK_RETURN, true); break;
                    case SDL_CONTROLLER_BUTTON_B:          push_key(SDLK_ESCAPE, true); break;
                    case SDL_CONTROLLER_BUTTON_START:      push_key(SDLK_SPACE, true); break;
                    default: break;
                }
            }
        }
        if (quit) break;

        wmc.begin_frame();
        WmcInput in;

        if (mode == Mode::Hub) {
            in = wmc.render_hub(discs, selected_disc, selected_stream, now());
            if (in.action == WmcAction::Play && !disc.streams.empty()) {
                const StreamItem& s = disc.streams[selected_stream];
                std::cout << "[*] Playing stream: " << s.name << "\n";
                if (open_stream(s)) {
                    mode = Mode::Player;
                    paused = false;
                    osd_visible = true;
                } else {
                    std::cerr << "[-] Failed to open stream " << s.name << "\n";
                }
            }
        } else {
            if (!paused) {
                FrameType ft = decoder.read_frame(video_frame, audio_pcm);
                if (ft == FrameType::Eof) {
                    std::cout << "[*] Reached End of Stream.\n";
                    mode = Mode::Hub;
                    close_decoder();
                } else if (ft == FrameType::Video) {
                    ui.update_video_texture(video_frame);
                    elapsed += 1.0 / fps;
                    const uint32_t cur = SDL_GetTicks();
                    const uint32_t e = cur - last_frame_time;
                    if (e < frame_delay_ms) SDL_Delay(frame_delay_ms - e);
                    last_frame_time = SDL_GetTicks();
                } else if (ft == FrameType::Audio) {
                    if (!audio_pcm.empty()) {
                        ui.queue_audio(audio_pcm.data(),
                                       static_cast<uint32_t>(audio_pcm.size()));
                    }
                }
            } else {
                SDL_Delay(10);
            }

            in = wmc.render_player(ui.video_texture(), ui.video_width(), ui.video_height(),
                                   disc, disc.streams[selected_stream].name,
                                   paused, elapsed, total, osd_visible, now());

            switch (in.action) {
                case WmcAction::TogglePause: paused = !paused; break;
                case WmcAction::SeekBackward:
                    decoder.seek_relative(-5.0);
                    elapsed = std::max(0.0, elapsed - 5.0);
                    break;
                case WmcAction::SeekForward:
                    decoder.seek_relative(5.0);
                    elapsed += 5.0;
                    break;
                case WmcAction::SeekTo:
                    decoder.seek_relative(in.seek_seconds - elapsed);
                    elapsed = in.seek_seconds;
                    break;
                case WmcAction::BackToHub:
                    mode = Mode::Hub;
                    close_decoder();
                    break;
                default: break;
            }
        }

        wmc.render();

        // Screenshot: let animations settle for a few frames, then read the
        // renderer backbuffer and write it out. Works in Hub and Player mode.
        if (!opt.screenshot_path.empty()) {
            if (shot_countdown < 0) shot_countdown = 30;
            if (--shot_countdown <= 0) {
                int ww = 0, wh = 0;
                SDL_GetRendererOutputSize(ui.renderer(), &ww, &wh);
                std::vector<uint8_t> px(static_cast<size_t>(ww) * wh * 4);
                if (SDL_RenderReadPixels(ui.renderer(), nullptr,
                                         SDL_PIXELFORMAT_ABGR8888,
                                         px.data(), ww * 4) == 0) {
                    stbi_write_png(opt.screenshot_path.c_str(), ww, wh, 4,
                                   px.data(), ww * 4);
                    std::cout << "[+] Screenshot saved: " << opt.screenshot_path
                              << " (" << ww << "x" << wh << ")\n";
                } else {
                    std::cerr << "[-] SDL_RenderReadPixels failed: "
                              << SDL_GetError() << "\n";
                }
                quit = true;
            }
        }

        wmc.present();
    }

    if (pad) SDL_GameControllerClose(pad);
    av_frame_free(&video_frame);
    close_decoder();
    wmc.close();
    ui.close();

    std::cout << "[+] Exited cleanly.\n";
    return 0;
}
