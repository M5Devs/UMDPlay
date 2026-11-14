#include "iso_reader.h"
#include "decoder.h"
#include "ui_manager.h"
#include "wmc_ui.h"
#include "clip_export.h"
#include "recent.h"

#include "stb_image_write.h"   // declarations only (impl is in stb_impl.cpp)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

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
    bool        fullscreen    = false;   // --fullscreen: start in fullscreen
    std::string start_strip;             // --strip recent|browse|settings
    bool        demo_player   = false;   // --demo-player: render the OSD, no ISO
};

void print_usage() {
    std::cout <<
        "Usage: umdplay [path_to_umd_video.iso] [options]\n"
        "  --play                 start the main feature immediately\n"
        "  --export-clip <mp4>    decode a clip to MP4 (H.264/AAC) and exit\n"
        "  --clip-seconds <n>     clip length in seconds (default 12)\n"
        "  --screenshot <png>     render a frame, write a PNG, then exit\n"
        "  --start <seconds>      start at this offset\n"
        "  --fullscreen           start in fullscreen (F11 / Alt+Enter toggles)\n"
        "  --strip <name>         start on a hub strip: recent|browse|settings\n"
        "  --probe                print stream/metadata info and exit\n"
        "  --demo                 render the hub with synthetic data (no ISO)\n"
        "  --demo-player          render the player OSD with synthetic data (no ISO)\n";
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
        } else if (a == "--demo-player") {
            opt.demo = true;
            opt.demo_player = true;
        } else if (a == "--fullscreen") {
            opt.fullscreen = true;
        } else if (a == "--strip" && i + 1 < argc) {
            opt.start_strip = argv[++i];
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

// A disc in the library: parsed metadata plus the raw MPS entries needed to
// open a stream later.
struct LibraryItem {
    DiscItem              disc;
    std::vector<MpsEntry> entries;
};

// Parse an ISO into a LibraryItem. Textures are created later by the caller.
bool load_iso(const std::string& path, LibraryItem& out) {
    IsoReader reader(path);
    if (!reader.open()) return false;

    auto entries = reader.get_mps_entries();
    if (entries.empty()) return false;

    DiscItem& d = out.disc;
    d.iso_path = path;
    d.title    = reader.param().title;
    d.disc_id  = reader.param().disc_id;
    d.category = reader.param().category;

    uint32_t max_size = 0;
    for (const auto& e : entries) max_size = std::max(max_size, e.size_bytes);
    for (const auto& e : entries) {
        StreamItem s;
        s.name       = e.name;
        s.size_bytes = e.size_bytes;
        s.is_main    = (e.size_bytes == max_size);
        d.streams.push_back(s);
    }
    out.entries = std::move(entries);
    return true;
}

// Load ICON0.PNG / PIC1.PNG for a disc into SDL textures via the WMC context.
void load_disc_textures(const std::string& iso_path, DiscItem& disc, WmcUi& wmc) {
    IsoReader reader(iso_path);
    if (!reader.open()) return;
    if (reader.has_metadata("ICON0.PNG")) {
        auto bytes = reader.read_metadata("ICON0.PNG");
        if (!bytes.empty()) disc.icon = wmc.texture_from_png(bytes);
    }
    if (reader.has_metadata("PIC1.PNG")) {
        auto bytes = reader.read_metadata("PIC1.PNG");
        if (!bytes.empty()) disc.pic1 = wmc.texture_from_png(bytes);
    }
}

void destroy_disc_textures(DiscItem& d) {
    if (d.icon) { SDL_DestroyTexture(d.icon); d.icon = nullptr; }
    if (d.pic1) { SDL_DestroyTexture(d.pic1); d.pic1 = nullptr; }
}

std::string parent_dir(const std::string& dir) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(dir, ec);
    if (ec || !p.has_parent_path() || p.parent_path() == p) return dir;
    return p.parent_path().string();
}

// Build the Browse System listing: parent entry, then sub-folders, then *.iso.
void build_browse(const std::string& dir, std::vector<BrowseEntry>& out) {
    out.clear();

    const fs::path p(dir);
    const std::string parent = parent_dir(dir);
    if (parent != dir) out.push_back({".. (up one level)", parent, true, false});

    std::vector<BrowseEntry> dirs, isos;
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const std::string name = de.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        std::error_code tec;
        if (de.is_directory(tec) && !tec) {
            dirs.push_back({name, de.path().string(), true, false});
        } else {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".iso")
                isos.push_back({name, de.path().string(), false, true});
        }
    }
    auto by_name = [](const BrowseEntry& a, const BrowseEntry& b) { return a.name < b.name; };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(isos.begin(), isos.end(), by_name);
    out.insert(out.end(), dirs.begin(), dirs.end());
    out.insert(out.end(), isos.begin(), isos.end());
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

    // --- Build the initial library -------------------------------------------
    std::vector<LibraryItem> recent_items;
    LibraryItem active;              // the disc currently selected / playing
    bool has_active = false;

    if (opt.demo) {
        DiscItem d;
        d.title    = "\u30c7\u30e2 UMD Video (Demo)";
        d.disc_id  = "UMDPLAY-DEMO";
        d.category = "UMD_VIDEO";
        d.is_demo  = true;
        const char* names[] = {"00001.MPS", "00002.MPS", "00003.MPS"};
        const uint32_t sizes[] = {1500000000u, 4000000u, 1200000u};
        for (int i = 0; i < 3; ++i) {
            StreamItem s;
            s.name = names[i];
            s.size_bytes = sizes[i];
            s.is_main = (i == 0);
            d.streams.push_back(s);
        }
        LibraryItem it;
        it.disc = d;
        recent_items.push_back(it);
        active = it;
        has_active = true;
    } else {
        // Restore previously opened discs from the recent list.
        for (const auto& path : recent::load()) {
            std::error_code ec;
            if (!fs::exists(path, ec) || ec) continue;
            LibraryItem it;
            if (load_iso(path, it)) recent_items.push_back(std::move(it));
        }

        if (!opt.iso_path.empty()) {
            LibraryItem it;
            std::cout << "[*] Inspecting ISO: " << opt.iso_path << "\n";
            if (!load_iso(opt.iso_path, it)) {
                std::cerr << "[-] Failed to read UMD Video filesystem!\n";
                return 1;
            }
            active = it;
            has_active = true;
        }
    }

    if (has_active) print_disc_summary(active.disc);

    // Headless clip export: no window needed, encode straight from the decoder.
    if (!opt.export_path.empty()) {
        if (!has_active || opt.demo || active.entries.empty()) {
            std::cerr << "[-] --export-clip needs a real ISO (not --demo).\n";
            return 1;
        }
        MpsEntry main_entry = active.entries[0];
        for (const auto& e : active.entries) {
            if (e.size_bytes > main_entry.size_bytes) main_entry = e;
        }
        std::cout << "[*] Exporting " << opt.clip_seconds << "s clip from "
                  << main_entry.name << " starting at " << opt.start_seconds << "s\n";
        const bool ok = export_clip(active.disc.iso_path, main_entry, opt.start_seconds,
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
    ui.set_fullscreen(opt.fullscreen);

    WmcUi wmc;
    std::string glyph_sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                               "0123456789 .,:;!?/()-+%[]()'\"&|\u2022\u2013\u2014";
    for (const auto& it : recent_items) glyph_sample += it.disc.title + it.disc.disc_id;
    if (has_active) glyph_sample += active.disc.title;

    if (!wmc.init(ui.window(), ui.renderer(), glyph_sample)) {
        std::cerr << "[-] Failed to initialize WMC UI (ImGui)!\n";
        ui.close();
        return 1;
    }

    // Decode cover art. `recent_items` is the single owner of all textures;
    // `active` is a metadata-only copy so nothing is freed twice.
    for (auto& it : recent_items) {
        if (!it.disc.iso_path.empty()) load_disc_textures(it.disc.iso_path, it.disc, wmc);
    }
    if (has_active && !active.disc.iso_path.empty()) {
        recent::add(active.disc.iso_path);
        int found = -1;
        for (int i = 0; i < static_cast<int>(recent_items.size()); ++i)
            if (recent_items[i].disc.iso_path == active.disc.iso_path) { found = i; break; }
        if (found >= 0) {
            LibraryItem ex = std::move(recent_items[found]);
            recent_items.erase(recent_items.begin() + found);
            recent_items.insert(recent_items.begin(), std::move(ex));
        } else {
            recent_items.insert(recent_items.begin(), active);
            load_disc_textures(active.disc.iso_path, recent_items.front().disc, wmc);
        }
        active = recent_items.front();
    }
    active.disc.icon = nullptr;
    active.disc.pic1 = nullptr;

    if (opt.probe_only) {
        for (auto& it : recent_items) destroy_disc_textures(it.disc);
        wmc.close();
        ui.close();
        return 0;
    }

    // --- Hub state ---
    std::vector<DiscItem> recent_cards;
    auto refresh_cards = [&]() {
        recent_cards.clear();
        for (const auto& it : recent_items) recent_cards.push_back(it.disc);
    };
    refresh_cards();

    int recent_sel = 0;
    int browse_sel = 0;
    int action_sel = 0;
    int extras_sel = 0;
    int setting_sel = 0;
    bool extras_open = false;

    std::string browse_dir;
    {
        std::error_code ec;
        if (has_active && !active.disc.iso_path.empty()) {
            browse_dir = fs::path(active.disc.iso_path).parent_path().string();
        }
        if (browse_dir.empty()) browse_dir = fs::current_path(ec).string();
        if (ec || browse_dir.empty()) browse_dir = ".";
    }
    std::vector<BrowseEntry> browse_entries;
    build_browse(browse_dir, browse_entries);

    HubMode strip = recent_cards.empty() ? HubMode::Browse : HubMode::Recent;
    if (opt.start_strip == "browse") strip = HubMode::Browse;
    else if (opt.start_strip == "settings") strip = HubMode::Settings;
    else if (opt.start_strip == "recent") strip = HubMode::Recent;

    // --- Playback state ---
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
    int play_stream_index = 0;

    // --demo-player: show the transport OSD with a synthetic video texture and
    // a fake clock so the OSD can be rendered (and screenshotted) with no ISO.
    std::vector<uint8_t> demo_yuv;
    if (opt.demo_player) {
        const int dw = 720, dh = 480;
        demo_yuv.assign(static_cast<size_t>(dw) * dh * 3 / 2, 0);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                demo_yuv[static_cast<size_t>(y) * dw + x] =
                    static_cast<uint8_t>((x + y) / 4 + 16);
        for (size_t i = static_cast<size_t>(dw) * dh; i < demo_yuv.size(); ++i)
            demo_yuv[i] = 128;
        mode = Mode::Player;
        paused = false;
        elapsed = 2130.0;   // 00:35:30
        total   = 7103.0;   // 01:58:23
    }

    SDL_GameController* pad = nullptr;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) { pad = SDL_GameControllerOpen(i); break; }
    }

    const float t0 = static_cast<float>(SDL_GetTicks()) / 1000.0f;
    auto now = [&]() { return static_cast<float>(SDL_GetTicks()) / 1000.0f - t0; };

    auto close_decoder = [&]() {
        if (decoder_open) { decoder.close(); decoder_open = false; }
    };

    // Open a stream of the active disc (uses active.entries for the LBA).
    auto open_stream = [&](int stream_idx) -> bool {
        if (!has_active || opt.demo || active.entries.empty()) return false;
        if (stream_idx < 0 || stream_idx >= static_cast<int>(active.disc.streams.size())) return false;
        const StreamItem& s = active.disc.streams[stream_idx];

        MpsEntry e;
        e.name = s.name;
        e.size_bytes = s.size_bytes;
        e.lba = 0;
        for (const auto& m : active.entries) {
            if (m.name == s.name) { e.lba = m.lba; break; }
        }
        close_decoder();
        if (!decoder.open_stream(active.disc.iso_path, e)) return false;
        decoder_open = true;
        fps = decoder.get_fps();
        if (fps <= 0.0 || fps > 120.0) fps = 29.97;
        frame_delay_ms = static_cast<uint32_t>(1000.0 / fps);
        total = decoder.get_duration();
        elapsed = 0.0;
        if (opt.start_seconds > 0.0) {
            decoder.seek_absolute(opt.start_seconds);
            elapsed = decoder.current_time();
        }
        last_frame_time = SDL_GetTicks();
        play_stream_index = stream_idx;
        return true;
    };

    auto main_stream_index = [&]() -> int {
        if (!has_active) return 0;
        for (size_t i = 0; i < active.disc.streams.size(); ++i)
            if (active.disc.streams[i].is_main) return static_cast<int>(i);
        return 0;
    };

    // Open an ISO chosen from Recent / Browse: load it, add to Recent, show it.
    auto open_iso_path = [&](const std::string& path) {
        LibraryItem it;
        if (!load_iso(path, it)) {
            std::cerr << "[-] Failed to open ISO: " << path << "\n";
            return;
        }
        recent::add(path);
        print_disc_summary(it.disc);

        // Reuse the existing library entry's textures if we already have them.
        int found = -1;
        for (int i = 0; i < static_cast<int>(recent_items.size()); ++i)
            if (recent_items[i].disc.iso_path == path) { found = i; break; }
        if (found >= 0) {
            LibraryItem ex = std::move(recent_items[found]);
            ex.disc.streams = it.disc.streams;   // refresh stream list
            recent_items.erase(recent_items.begin() + found);
            recent_items.insert(recent_items.begin(), std::move(ex));
        } else {
            recent_items.insert(recent_items.begin(), it);
            load_disc_textures(path, recent_items.front().disc, wmc);
        }
        active = recent_items.front();
        active.disc.icon = nullptr;
        active.disc.pic1 = nullptr;
        has_active = true;

        refresh_cards();
        recent_sel = 0;
        action_sel = 0;
        extras_open = false;
        strip = HubMode::Recent;
    };

    if (opt.autoplay && has_active) {
        const int idx = main_stream_index();
        if (open_stream(idx)) {
            mode = Mode::Player;
            std::cout << "[*] Autoplay: " << active.disc.streams[idx].name << "\n";
        }
    }

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            wmc.process_event(ev);
            if (ev.type == SDL_QUIT) quit = true;
            if (ev.type == SDL_KEYDOWN) {
                const SDL_Keycode k = ev.key.keysym.sym;
                const bool alt = (ev.key.keysym.mod & KMOD_ALT) != 0;
                if (k == SDLK_q && mode == Mode::Hub) quit = true;
                if (k == SDLK_F11 || (k == SDLK_RETURN && alt)) ui.toggle_fullscreen();
            }
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
            in = wmc.render_hub(strip, recent_cards, recent_sel,
                                browse_entries, browse_sel, browse_dir,
                                action_sel, extras_open, extras_sel, setting_sel,
                                ui.is_fullscreen(), ui.volume(), ui.muted(), now());

            switch (in.action) {
                case WmcAction::OpenIso:
                    open_iso_path(in.iso_path);
                    break;
                case WmcAction::BrowseEnter:
                    browse_dir = in.iso_path;
                    build_browse(browse_dir, browse_entries);
                    browse_sel = 0;
                    break;
                case WmcAction::BrowseUp:
                    browse_dir = parent_dir(browse_dir);
                    build_browse(browse_dir, browse_entries);
                    browse_sel = 0;
                    break;
                case WmcAction::Play: {
                    const int idx = main_stream_index();
                    std::cout << "[*] Playing main feature: " << active.disc.streams[idx].name << "\n";
                    if (open_stream(idx)) { mode = Mode::Player; paused = false; osd_visible = true; }
                    else std::cerr << "[-] Failed to open main stream\n";
                    break;
                }
                case WmcAction::PlayStream:
                    if (in.stream_index >= 0) {
                        std::cout << "[*] Playing bonus stream: "
                                  << active.disc.streams[in.stream_index].name << "\n";
                        if (open_stream(in.stream_index)) { mode = Mode::Player; paused = false; osd_visible = true; }
                    }
                    break;
                case WmcAction::ToggleFullscreen:
                    ui.toggle_fullscreen();
                    break;
                case WmcAction::VolumeChanged:
                    ui.set_volume(in.volume);
                    break;
                case WmcAction::MuteToggled:
                    ui.set_muted(!ui.muted());
                    break;
                case WmcAction::Quit:
                    quit = true;
                    break;
                default: break;
            }
        } else {
            if (opt.demo_player) {
                ui.update_demo_texture(demo_yuv.data(), 720, 480);
            } else if (!paused) {
                FrameType ft = decoder.read_frame(video_frame, audio_pcm);
                if (ft == FrameType::Eof) {
                    std::cout << "[*] Reached End of Stream.\n";
                    mode = Mode::Hub;
                    close_decoder();
                    ui.clear_audio_queue();
                    ui.pause_audio(false);
                } else if (ft == FrameType::Video) {
                    ui.update_video_texture(video_frame);
                    elapsed = decoder.current_time();
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

            const std::string sname =
                (play_stream_index >= 0 &&
                 play_stream_index < static_cast<int>(active.disc.streams.size()))
                    ? active.disc.streams[play_stream_index].name
                    : std::string("stream");
            in = wmc.render_player(ui.video_texture(), ui.video_width(), ui.video_height(),
                                   active.disc, sname, paused, elapsed, total, osd_visible,
                                   ui.volume(), ui.muted(), ui.is_fullscreen(), now());

            switch (in.action) {
                case WmcAction::TogglePause:
                    paused = !paused;
                    ui.pause_audio(paused);
                    break;
                case WmcAction::SeekRelative:
                case WmcAction::SeekBackward:
                case WmcAction::SeekForward: {
                    const double delta = (in.action == WmcAction::SeekRelative)
                                             ? in.seek_delta
                                             : (in.action == WmcAction::SeekBackward ? -10.0 : 10.0);
                    decoder.seek_absolute(elapsed + delta);
                    elapsed = decoder.current_time();
                    ui.clear_audio_queue();   // drop stale audio from the old position
                    break;
                }
                case WmcAction::SeekTo:
                    decoder.seek_absolute(in.seek_seconds);
                    elapsed = decoder.current_time();
                    ui.clear_audio_queue();
                    break;
                case WmcAction::VolumeChanged:
                    ui.set_volume(in.volume);
                    break;
                case WmcAction::MuteToggled:
                    ui.set_muted(!ui.muted());
                    break;
                case WmcAction::ToggleFullscreen:
                    ui.toggle_fullscreen();
                    break;
                case WmcAction::Stop:
                case WmcAction::BackToHub:
                    mode = Mode::Hub;
                    close_decoder();
                    ui.clear_audio_queue();
                    ui.pause_audio(false);
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

    for (auto& it : recent_items) destroy_disc_textures(it.disc);
    destroy_disc_textures(active.disc);
    if (pad) SDL_GameControllerClose(pad);
    av_frame_free(&video_frame);
    close_decoder();
    wmc.close();
    ui.close();

    std::cout << "[+] Exited cleanly.\n";
    return 0;
}
