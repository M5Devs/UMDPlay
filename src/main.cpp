#include "iso_reader.h"
#include "decoder.h"
#include "ui_manager.h"
#include "wmc_ui.h"
#include "clip_export.h"
#include "recent.h"
#include "playlist_parser.h"
#include "rco_parser.h"
#include "rco_menu.h"
#include "rco_texture.h"
#include "ui_sfx.h"
#include "vsmx_vm.h"

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

enum class Mode { Hub, Player, DiscMenu };

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
    bool        open_settings = false;   // --open-settings: open the Settings overlay
    bool        open_chapters = false;   // --open-chapters: open the Chapters panel
    bool        open_menu     = false;   // --open-menu: open the .RCO Disc Menu
    std::string menu_page;                // --menu-page <name>: start on a page
    std::string aspect_name;             // --aspect auto|16:9|4:3|fill
    bool        verbose = false;         // --verbose: full VSMX disassembly in --probe
    bool        no_ui_sounds = false;    // --no-ui-sounds: start with UI chimes off
};

void print_usage() {
    std::cout <<
        "Usage: umdplay [path_to_umd_video.iso|.chd] [options]\n"
        "  --play                 start the main feature immediately\n"
        "  --export-clip <mp4>    decode a clip to MP4 (H.264/AAC) and exit\n"
        "  --clip-seconds <n>     clip length in seconds (default 12)\n"
        "  --screenshot <png>     render a frame, write a PNG, then exit\n"
        "  --start <seconds>      start at this offset\n"
        "  --fullscreen           start in fullscreen (F11 / Alt+Enter toggles)\n"
        "  --strip <name>         start on a hub strip: recent|browse|settings\n"
        "  --probe                print stream/metadata info and exit\n"
        "  --verbose              with --probe: disassemble the menu VSMX script\n"
        "  --demo                 render the hub with synthetic data (no ISO)\n"
        "  --demo-player          render the player OSD with synthetic data (no ISO)\n"
        "  --open-settings        open the in-player Settings overlay (with --demo-player)\n"
        "  --open-chapters        open the Chapters panel (with --demo)\n"
        "  --open-menu            open the interactive .RCO Disc Menu\n"
        "  --menu-page <name>     start the Disc Menu on a named page\n"
        "  --aspect <mode>        Zoom mode: auto|16:9|4:3|fill\n"
        "  --no-ui-sounds         start with WMC UI chimes muted\n";
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
        } else if (a == "--verbose") {
            opt.verbose = true;
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
        } else if (a == "--open-settings") {
            opt.open_settings = true;
        } else if (a == "--open-chapters") {
            opt.open_chapters = true;
        } else if (a == "--open-menu") {
            opt.open_menu = true;
        } else if (a == "--menu-page" && i + 1 < argc) {
            opt.menu_page = argv[++i];
        } else if (a == "--aspect" && i + 1 < argc) {
            opt.aspect_name = argv[++i];
        } else if (a == "--no-ui-sounds") {
            opt.no_ui_sounds = true;
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
    std::cout << "    ICON0   : " << (d.has_icon ? "present" : "absent") << "\n";
    std::cout << "    PIC1    : " << (d.has_pic1 ? "present" : "absent") << "\n";
    std::cout << "    SND0    : " << (d.has_snd0 ? "present" : "absent") << "\n";
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

// Load PLAYLIST.UMD chapters (of the main feature) into a DiscItem, and stash
// the playlist header title. No-op when the disc has no PLAYLIST.UMD.
void load_playlist(IsoReader& reader, DiscItem& d) {
    const UmdAsset* pl = reader.find_asset("PLAYLIST.UMD");
    if (!pl) return;
    auto raw = reader.read_asset(*pl);
    if (raw.empty()) return;
    auto parsed = umdplay::parse_playlist(raw.data(), raw.size());
    if (!parsed.valid) return;
    d.playlist_title = parsed.title;

    // Pick the main feature track (most markers / longest) and expose its
    // markers as chapters. Prefer the track whose clip matches the main stream
    // when possible, so extras/still tracks don't win on a marker tie.
    const umdplay::PlaylistTrack* main = parsed.main_track();
    if (!main) return;
    for (const auto& s : d.streams) {
        if (!s.is_main) continue;
        // s.name is e.g. "00001.MPS"; the playlist clip id is "00001".
        std::string id = s.name;
        const size_t dot = id.find('.');
        if (dot != std::string::npos) id.resize(dot);
        for (const auto& t : parsed.tracks) {
            if (t.clip_name == id && !t.markers.empty()) { main = &t; break; }
        }
        break;
    }
    int idx = 0;
    for (const auto& mk : main->markers) {
        ChapterItem c;
        c.label   = mk.name.empty() ? ("Chapter " + std::to_string(idx + 1)) : mk.name;
        c.seconds = mk.seconds;
        d.chapters.push_back(std::move(c));
        ++idx;
    }
}

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
    d.has_icon = reader.has_metadata("ICON0.PNG");
    d.has_pic1 = reader.has_metadata("PIC1.PNG");
    d.has_snd0 = reader.has_metadata("SND0.AT3");
    for (const auto& a : reader.assets_in("RESOURCE")) {
        if (a.name.size() >= 4 &&
            a.name.compare(a.name.size() - 4, 4, ".RCO") == 0) { d.has_menu = true; break; }
    }

    uint32_t max_size = 0;
    for (const auto& e : entries) max_size = std::max(max_size, e.size_bytes);
    for (const auto& e : entries) {
        StreamItem s;
        s.name       = e.name;
        s.size_bytes = e.size_bytes;
        s.is_main    = (e.size_bytes == max_size);
        d.streams.push_back(s);
    }
    load_playlist(reader, d);   // real chapter markers (when present)
    out.entries = std::move(entries);
    return true;
}

// Extended --probe dump: PLAYLIST.UMD tracks/chapters + .RCO container summary.
void print_disc_structure(const std::string& path, bool opt_verbose) {
    IsoReader reader(path);
    if (!reader.open()) return;

    std::cout << "    Root    : " << reader.root_name() << "\n";
    std::cout << "    Assets  : " << reader.assets().size() << "\n";
    for (const auto& a : reader.assets()) {
        std::cout << "      - " << a.dir << "/" << a.name
                  << "  (" << a.size_bytes << " B)\n";
    }

    if (const UmdAsset* pl = reader.find_asset("PLAYLIST.UMD")) {
        auto raw = reader.read_asset(*pl);
        auto parsed = umdplay::parse_playlist(raw.data(), raw.size());
        std::cout << "    " << umdplay::playlist_summary(parsed);
        const auto* main = parsed.main_track();
        if (main) {
            int ti = static_cast<int>(main - &parsed.tracks[0]);
            auto ch = parsed.chapters(ti);
            std::cout << "    Chapters (main track '" << main->name << "', clip "
                      << main->clip_name << "): " << ch.size() << "\n";
            for (const auto& c : ch) {
                char ts[16];
                const int hh = static_cast<int>(c.seconds) / 3600;
                const int mm = (static_cast<int>(c.seconds) / 60) % 60;
                const int ss = static_cast<int>(c.seconds) % 60;
                std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", hh, mm, ss);
                std::cout << "      " << (c.index + 1) << ". " << ts
                          << "  " << c.label << "\n";
            }
        }
    }

    // Probe each RESOURCE/*.RCO: container summary, menu object tree and the
    // embedded VSMX script (decoded + histogram; disassembly with --verbose).
    for (const auto& a : reader.assets_in("RESOURCE")) {
        auto raw = reader.read_asset(a);
        auto rco = umdplay::parse_rco(raw.data(), raw.size());
        std::cout << "    " << a.name << ": " << umdplay::rco_summary(rco);

        auto pages = rco.pages();
        for (uint32_t page : pages) {
            const auto* p = rco.object(page);
            if (!p) continue;
            auto kids = rco.children_of(page);
            std::cout << "      page '" << p->name << "'  children="
                      << kids.size() << "\n";
            for (uint32_t kid : kids) {
                const auto* k = rco.object(kid);
                if (!k) continue;
                std::cout << "        - " << k->name << "  type=" << k->type;
                if (!k->on_push_event.empty())
                    std::cout << "  onPush=" << k->on_push_event;
                std::cout << "\n";
            }
        }

        if (rco.vsmx.present &&
            static_cast<uint64_t>(rco.vsmx.offset) + rco.vsmx.size <=
                raw.size()) {
            auto prog = umdplay::decode_vsmx(raw.data() + rco.vsmx.offset,
                                             rco.vsmx.size);
            if (prog.valid) {
                std::cout << "      VSMX '" << rco.vsmx.name << "': "
                          << prog.code.size() << " instructions, "
                          << prog.texts.size() << " strings, "
                          << prog.properties.size() << " props, "
                          << prog.names.size() << " names\n";
                std::cout << "        " << umdplay::vsmx_opcode_histogram(prog)
                          << "\n";
                if (opt_verbose)
                    std::cout << umdplay::disassemble(prog);
            }
        }
    }
}

// Current chapter for display: the parser's chapter_at, clamped to 0 when the
// playhead is still before the first marker (so "CH 1/45" shows from the start).
int current_chapter(const DiscItem& d, double t) {
    if (d.chapters.empty()) return -1;
    const int idx = umdplay::chapter_at(d.chapters, t);
    return idx < 0 ? 0 : idx;
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

// Build the Browse System listing: parent entry, then sub-folders, then disc
// images (*.iso / *.chd).
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
            if (umdplay::is_disc_image_path(name))
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
        // A handful of synthetic discs so the cross-axis carousel has something
        // to scroll through (titles deliberately mix Latin + Japanese so the CJK
        // glyph range gets exercised).
        struct DemoDisc { const char* title; const char* id; const char* cat; };
        const DemoDisc discs[] = {
            {"Demo UMD Video \u30c7\u30e2", "UMDPLAY-DEMO", "UMD_VIDEO"},
            {"Spider-Man 2",                "UMDS-00021",   "UMD_VIDEO"},
            {"Final Fantasy VII \u30a2\u30c9\u30d9\u30f3\u30c8", "UCJS-10001", "UMD_VIDEO"},
            {"Hitchhiker's Guide",          "UMDG-00007",   "UMD_VIDEO"},
            {"Cowboy Bebop \u30bb\u30c3\u30b7\u30e7\u30f3", "UMDA-00033", "UMD_AUDIO"},
        };
        const char* names[] = {"00001.MPS", "00002.MPS", "00003.MPS"};
        const uint32_t sizes[] = {1500000000u, 4000000u, 1200000u};
        for (const auto& dd : discs) {
            DiscItem d;
            d.title    = dd.title;
            d.disc_id  = dd.id;
            d.category = dd.cat;
            d.is_demo  = true;
            for (int i = 0; i < 3; ++i) {
                StreamItem s;
                s.name = names[i];
                s.size_bytes = sizes[i];
                s.is_main = (i == 0);
                d.streams.push_back(s);
            }
            // Synthetic chapter markers so the Chapters panel can be exercised
            // (and screenshotted) without a real disc.
            for (int i = 0; i < 6; ++i) {
                ChapterItem c;
                c.label   = "Mark" + std::to_string((i + 1) * 4515);
                c.seconds = i * 270.0;
                d.chapters.push_back(std::move(c));
            }
            LibraryItem it;
            it.disc = d;
            recent_items.push_back(it);
        }
        active = recent_items.front();
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
    if (opt.probe_only && has_active && !active.disc.iso_path.empty())
        print_disc_structure(active.disc.iso_path, opt.verbose);

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
    ui.set_window_icon();
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

    // Procedural WMC UI chimes (own SDL audio device; headless-safe).
    umdplay::UiSfx ui_sfx;
    ui_sfx.init();
    bool ui_sounds = !opt.no_ui_sounds;
    // UI chimes are gated by the Options switch and the player mute state.
    auto ui_cue = [&](umdplay::UiCue cue) {
        if (!ui_sounds) return;
        ui_sfx.set_muted(ui.muted());
        ui_sfx.play(cue);
    };

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
    int chapter_sel = 0;
    int setting_sel = 0;
    bool extras_open = false;
    bool chapters_open = false;
    if (opt.open_chapters) { chapters_open = true; action_sel = 1; }
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
    umdplay::VideoFrame video_frame;
    std::vector<uint8_t> audio_pcm;
    bool paused = false;
    bool osd_visible = true;
    bool settings_open = opt.open_settings;
    aspect::Mode aspect_mode = aspect::Mode::Auto;
    if (opt.aspect_name == "16:9" || opt.aspect_name == "16x9") aspect_mode = aspect::Mode::R16x9;
    else if (opt.aspect_name == "4:3" || opt.aspect_name == "4x3") aspect_mode = aspect::Mode::R4x3;
    else if (opt.aspect_name == "fill") aspect_mode = aspect::Mode::Fill;
    NowPlaying now_playing;   // hub PIP summary while a stream is loaded
    double elapsed = 0.0, total = 0.0;
    double fps = 29.97;
    uint32_t frame_delay_ms = 33;
    uint32_t last_frame_time = 0;
    bool decoder_open = false;
    bool quit = false;
    int shot_countdown = -1;
    int play_stream_index = 0;

    // --- Disc Menu state (interactive .RCO page navigation) ---
    umdplay::RcoMenu disc_menu;
    umdplay::RcoTextureCache menu_tex;
    bool disc_menu_open = false;
    auto open_disc_menu = [&]() -> bool {
        if (!has_active || !active.disc.has_menu || active.disc.iso_path.empty())
            return false;
        IsoReader reader(active.disc.iso_path);
        if (!reader.open()) return false;
        std::vector<uint8_t> best;
        std::string best_name;
        for (const auto& a : reader.assets_in("RESOURCE")) {
            if (a.name.size() < 4) continue;
            std::string ext = a.name.substr(a.name.size() - 4);
            for (char& ch : ext) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (ext != ".RCO") continue;
            auto raw = reader.read_asset(a);
            if (raw.size() > best.size()) { best = std::move(raw); best_name = a.name; }
        }
        if (best.empty()) return false;
        auto rco = umdplay::parse_rco(best.data(), best.size());
        const int seed_ch = std::max(1, current_chapter(active.disc, elapsed) + 1);
        if (!disc_menu.build(rco, 3, seed_ch)) {
            std::cerr << "[-] Disc Menu: " << best_name << " has no usable VSMX menu\n";
            return false;
        }
        menu_tex.build(ui.renderer(), rco, best.data(), best.size());
        disc_menu_open = true;
        std::cout << "[*] Disc Menu: " << best_name << " - page '"
                  << disc_menu.page_name() << "' (" << disc_menu.page_count()
                  << " pages, " << disc_menu.page().items.size() << " items, "
                  << menu_tex.count() << " textures)\n";
        return true;
    };

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
        chapters_open = false;
        strip = HubMode::Recent;
    };

    // --open-menu: jump straight into the Disc Menu (used for screenshots).
    if (opt.open_menu && has_active && !opt.demo) {
        if (open_disc_menu()) {
            if (!opt.menu_page.empty() && !disc_menu.goto_page(opt.menu_page))
                std::cerr << "[-] Disc Menu: no page named '" << opt.menu_page << "'\n";
            mode = Mode::DiscMenu;
        }
    }

    if (opt.autoplay && has_active) {
        const int idx = main_stream_index();
        if (open_stream(idx)) {
            mode = Mode::Player;
            std::cout << "[*] Autoplay: " << active.disc.streams[idx].name << "\n";
        }
    }

    // Advance one playback tick: decode the next video frame / audio chunk,
    // update the video texture and clock. Runs in both Hub and Player modes so
    // the hub's Now Playing PIP keeps showing live video. Returns false when the
    // stream ended (the caller drops back to the Hub).
    auto advance_frame = [&]() -> bool {
        if (opt.demo_player) {
            ui.update_demo_texture(demo_yuv.data(), 720, 480);
            return true;
        }
        if (!decoder_open || paused) { SDL_Delay(decoder_open ? 8 : 0); return true; }

        FrameType ft = decoder.read_frame(video_frame, audio_pcm);
        if (ft == FrameType::Eof) {
            std::cout << "[*] Reached End of Stream.\n";
            close_decoder();
            ui.clear_audio_queue();
            ui.pause_audio(false);
            return false;
        }
        if (ft == FrameType::Video) {
            ui.update_video_texture(video_frame);
            elapsed = decoder.current_time();
            const uint32_t cur = SDL_GetTicks();
            const uint32_t e = cur - last_frame_time;
            if (e < frame_delay_ms) SDL_Delay(frame_delay_ms - e);
            last_frame_time = SDL_GetTicks();
        } else if (ft == FrameType::Audio) {
            if (!audio_pcm.empty())
                ui.queue_audio(audio_pcm.data(), static_cast<uint32_t>(audio_pcm.size()));
        }
        return true;
    };

    // Welcoming chime on launch. Gated by the UI Sounds switch; on a machine
    // with no audio device play() is a no-op, so headless demos are unaffected.
    ui_cue(umdplay::UiCue::Startup);

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
                // Disc Menu entry: "T" (top menu) in the Hub/Player, or the
                // disc's own Menu button. "M"/"m" stays the player's mute
                // toggle, and ESC inside the menu is handled by the menu itself
                // (step back a page, or leave at the top level).
                if (k == SDLK_t && mode != Mode::DiscMenu &&
                    active.disc.has_menu && !opt.demo) {
                    open_disc_menu();
                    mode = Mode::DiscMenu;
                }
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
            now_playing.active = decoder_open;
            now_playing.paused = paused;
            now_playing.elapsed = elapsed;
            now_playing.total = total;
            if (has_active) {
                now_playing.title = active.disc.title;
                if (play_stream_index >= 0 &&
                    play_stream_index < static_cast<int>(active.disc.streams.size()))
                    now_playing.stream = active.disc.streams[play_stream_index].name;
            }
            in = wmc.render_hub(ui.video_texture(), ui.video_width(), ui.video_height(),
                                recent_cards, recent_sel,
                                browse_entries, browse_sel, browse_dir,
                                strip, action_sel, extras_open, extras_sel,
                                chapters_open, chapter_sel, setting_sel,
                                now_playing, /*loading=*/false,
                                ui.is_fullscreen(), ui.volume(), ui.muted(), ui_sounds, now());
            ui_cue(in.cue);

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
                case WmcAction::BackToPlayback:
                    // Resume fullscreen playback from the hub's Now Playing PIP.
                    if (decoder_open) { mode = Mode::Player; osd_visible = true; }
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
                case WmcAction::PlayChapter:
                    // Jump straight to a chapter: open the main stream, seek to
                    // the marker, then switch to fullscreen playback.
                    if (!active.disc.chapters.empty() && in.chapter_index >= 0 &&
                        in.chapter_index < static_cast<int>(active.disc.chapters.size())) {
                        const double target = active.disc.chapters[in.chapter_index].seconds;
                        std::cout << "[*] Jumping to chapter " << (in.chapter_index + 1)
                                  << " @ " << target << "s\n";
                        if (!decoder_open) open_stream(main_stream_index());
                        if (decoder_open) {
                            decoder.seek_absolute(target);
                            elapsed = decoder.current_time();
                            ui.clear_audio_queue();
                            mode = Mode::Player; paused = false; osd_visible = true;
                        }
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
                case WmcAction::ToggleUiSounds:
                    ui_sounds = !ui_sounds;
                    // Audible confirmation when switching the chimes back on.
                    if (ui_sounds) { ui_sfx.set_muted(ui.muted()); ui_sfx.play(umdplay::UiCue::Select); }
                    break;
                case WmcAction::Quit:
                    quit = true;
                    break;
                default: break;
            }
            // Keep the stream advancing in the background so the hub's Now
            // Playing PIP shows live video (and audio stays in sync).
            if (decoder_open) advance_frame();
        } else if (mode == Mode::DiscMenu) {
            DiscMenuResult dr = wmc.render_disc_menu(disc_menu, menu_tex,
                                                     active.disc, now());
            ui_cue(dr.cue);
            if (dr.close) mode = decoder_open ? Mode::Player : Mode::Hub;
            if (dr.action.kind != umdplay::MenuActionKind::None) {
                switch (dr.action.kind) {
                    case umdplay::MenuActionKind::PlayMovie: {
                        std::cout << "[*] Disc Menu -> play";
                        if (dr.action.chapter > 0)
                            std::cout << " chapter " << dr.action.chapter;
                        std::cout << "\n";
                        const int idx = main_stream_index();
                        if (open_stream(idx)) {
                            if (dr.action.chapter > 0 &&
                                dr.action.chapter <= static_cast<int>(active.disc.chapters.size())) {
                                const double target = active.disc.chapters[dr.action.chapter - 1].seconds;
                                decoder.seek_absolute(target);
                                elapsed = decoder.current_time();
                                ui.clear_audio_queue();
                            }
                            mode = Mode::Player; paused = false; osd_visible = true;
                        }
                        break;
                    }
                    case umdplay::MenuActionKind::BackToPlayer:
                        mode = Mode::Player;
                        break;
                    case umdplay::MenuActionKind::ExitMenu:
                        mode = decoder_open ? Mode::Player : Mode::Hub;
                        break;
                    default: break;
                }
            }
        } else {
            if (!advance_frame()) mode = Mode::Hub;

            if (mode == Mode::Player) {
                const std::string sname =
                    (play_stream_index >= 0 &&
                     play_stream_index < static_cast<int>(active.disc.streams.size()))
                        ? active.disc.streams[play_stream_index].name
                        : std::string("stream");

                PlayerMediaInfo mi;
                mi.sfo_title    = active.disc.title;
                mi.video_codec  = decoder.video_codec_name();
                mi.video_w      = ui.video_width();
                mi.video_h      = ui.video_height();
                mi.fps          = fps;
                mi.par          = decoder.video_par();
                mi.stride       = decoder.audio_stride();
                mi.psmf_audio   = decoder.using_psmf_audio();
                mi.sample_rate  = decoder.audio_sample_rate();
                mi.deinterlace  = decoder.deinterlace_enabled();
                mi.interlaced   = decoder.last_frame_interlaced();
                mi.chapter_count = static_cast<int>(active.disc.chapters.size());
                mi.chapter       = current_chapter(active.disc, elapsed);
                if (mi.chapter >= 0)
                    mi.chapter_label = active.disc.chapters[mi.chapter].label;

                bool menu_clicked = false;
                in = wmc.render_player(ui.video_texture(), ui.video_width(), ui.video_height(),
                                       active.disc, sname, paused, elapsed, total, osd_visible,
                                       settings_open, setting_sel, aspect_mode, mi,
                                       ui.volume(), ui.muted(), ui.is_fullscreen(),
                                       active.disc.has_menu && !opt.demo, ui_sounds, menu_clicked, now());
                ui_cue(in.cue);

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
                    case WmcAction::PrevChapter:
                    case WmcAction::NextChapter: {
                        const int dir = (in.action == WmcAction::PrevChapter) ? -1 : 1;
                        const int ci = umdplay::chapter_step(active.disc.chapters, elapsed, dir);
                        if (ci >= 0) {
                            const double target = active.disc.chapters[ci].seconds;
                            decoder.seek_absolute(target);
                            elapsed = decoder.current_time();
                            ui.clear_audio_queue();
                            std::cout << "[*] Chapter " << (ci + 1) << "/"
                                      << active.disc.chapters.size() << " @ "
                                      << target << "s ("
                                      << active.disc.chapters[ci].label << ")\n";
                        }
                        break;
                    }
                    case WmcAction::ToggleDeinterlace:
                        decoder.set_deinterlace(!decoder.deinterlace_enabled());
                        break;
                    case WmcAction::SetAspect:
                        aspect_mode = in.aspect_mode;
                        break;
                    case WmcAction::VolumeChanged:
                        ui.set_volume(in.volume);
                        break;
                    case WmcAction::MuteToggled:
                        ui.set_muted(!ui.muted());
                        break;
                    case WmcAction::ToggleUiSounds:
                        ui_sounds = !ui_sounds;
                        if (ui_sounds) { ui_sfx.set_muted(ui.muted()); ui_sfx.play(umdplay::UiCue::Select); }
                        break;
                    case WmcAction::ToggleFullscreen:
                        ui.toggle_fullscreen();
                        break;
                    case WmcAction::Stop:
                        // Stop decoding but stay in the Player with the frame
                        // frozen; Esc / the HUB button return to the Hub.
                        close_decoder();
                        ui.clear_audio_queue();
                        ui.pause_audio(false);
                        break;
                    case WmcAction::BackToHub:
                        mode = Mode::Hub;
                        break;
                    case WmcAction::OpenDiscMenu:
                        if (open_disc_menu()) mode = Mode::DiscMenu;
                        break;
                    default: break;
                }
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
    close_decoder();
    ui_sfx.close();
    wmc.close();
    ui.close();

    std::cout << "[+] Exited cleanly.\n";
    return 0;
}
