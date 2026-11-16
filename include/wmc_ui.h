#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

#include "aspect.h"
#include "gesture.h"
#include "rco_menu.h"
#include "rco_texture.h"
#include "ui_sfx.h"

struct ImFont;
struct ImDrawList;

// One playable stream inside a disc (an .MPS file).
struct StreamItem {
    std::string name;
    uint32_t    size_bytes = 0;
    bool        is_main    = false;
};

// One chapter / marker inside a disc's main feature (from PLAYLIST.UMD).
struct ChapterItem {
    std::string label;         // "Mark4515" / "Chapter 1" (never empty)
    double      seconds = 0.0; // absolute timestamp within the stream
};

// A disc (one ISO) shown as a carousel tile.
struct DiscItem {
    std::string              iso_path;
    std::string              title;      // PARAM.SFO TITLE
    std::string              disc_id;    // PARAM.SFO DISC_ID
    std::string              category;   // PARAM.SFO CATEGORY (UV / UA / UG)
    std::vector<StreamItem>  streams;
    std::vector<ChapterItem> chapters;   // PLAYLIST.UMD markers of the main track
    std::string              playlist_title;  // PLAYLIST.UMD header title

    SDL_Texture*             icon = nullptr;   // ICON0.PNG
    SDL_Texture*             pic1 = nullptr;   // PIC1.PNG (optional)

    // Presence of the on-disc metadata files (independent of texture loading,
    // so headless --probe reports them correctly).
    bool has_icon = false;                     // ICON0.PNG
    bool has_pic1 = false;                     // PIC1.PNG
    bool has_snd0 = false;                     // SND0.AT3
    bool has_menu = false;                     // a RESOURCE/*.RCO menu is present

    bool is_demo = false;                      // synthetic entry (--demo)
};

// What the hub is currently showing. `NowPlaying` is the cross-axis category
// that returns to fullscreen playback and only appears while a stream is open.
enum class HubMode {
    Recent,       // the Recent Discs category
    Browse,       // the Browse System (folder/file) category
    Settings,     // the Options category
    NowPlaying    // jump back into playback (only while a stream is open)
};

// What the Disc Menu screen asks main() to do after an interaction.
struct DiscMenuResult {
    umdplay::MenuAction action;   // from a button press (may be None)
    bool close = false;           // leave the menu (Esc at the top level)
    umdplay::UiCue cue = umdplay::UiCue::None;  // UI sound to play this frame
};

// One entry in the Browse System listing (a folder or a .iso file).
struct BrowseEntry {
    std::string name;
    std::string path;
    bool        is_dir = false;
    bool        is_iso = false;
};

// Live "Now Playing" summary for the hub PIP card.
struct NowPlaying {
    bool        active = false;
    bool        paused = false;
    std::string title;
    std::string stream;
    double      elapsed = 0.0;
    double      total   = 0.0;
    int         chapter = -1;   // current chapter index (-1 = none)
    int         chapters = 0;   // total chapters on the disc
};

// Static media facts for the in-player Settings "Media Info" readout.
struct PlayerMediaInfo {
    std::string sfo_title;
    std::string video_codec;
    int         video_w = 0, video_h = 0;
    double      fps = 0.0;
    float       par = 1.0f;
    size_t      stride = 0;
    bool        psmf_audio = false;
    int         sample_rate = 0;
    bool        deinterlace = true;
    bool        interlaced = false;
    int         chapter = -1;      // current chapter index (-1 = none)
    int         chapter_count = 0; // total chapters
    std::string chapter_label;     // current chapter label
};

// Navigation intents emitted by the UI, consumed by main().
enum class WmcAction {
    None,
    Quit,
    Play,               // Enter on the main feature
    PlayStream,         // Enter on a bonus stream (index in `stream_index`)
    OpenIso,            // open the .iso at `iso_path` (Recent / Browse)
    BrowseEnter,        // descend into the folder at `iso_path`
    BrowseUp,           // go to the parent folder
    BackToHub,          // Esc in player mode
    BackToPlayback,     // hub PIP / transport: resume fullscreen playback
    TogglePause,
    SeekBackward,
    SeekForward,
    SeekRelative,       // seek by `seek_delta` seconds (negative = back)
    Stop,               // stop playback, return to the Hub
    SeekTo,             // seek bar drag (absolute seconds in `seek_seconds`)
    PrevChapter,        // jump to the previous PLAYLIST.UMD marker
    NextChapter,        // jump to the next PLAYLIST.UMD marker
    PlayChapter,        // jump to chapter `chapter_index` (hub chapter list)
    ToggleFullscreen,
    ToggleDeinterlace,
    SetAspect,          // new zoom mode in `aspect_mode`
    VolumeChanged,      // new gain in `volume`
    MuteToggled,
    ToggleUiSounds,     // Options: flip the UI Sounds on/off switch
    OpenDiscMenu        // open the interactive .RCO Disc Menu
};

struct WmcInput {
    WmcAction action = WmcAction::None;
    double    seek_seconds = 0.0;
    double    seek_delta = 0.0;
    int       stream_index = -1;
    int       chapter_index = -1;
    float     volume = 1.0f;
    std::string iso_path;
    aspect::Mode aspect_mode = aspect::Mode::Auto;
    umdplay::UiCue cue = umdplay::UiCue::None;  // UI sound to play this frame
};

// Windows Media Center inspired 10-foot interface. A signature WMC cross-axis
// hub (vertical categories + horizontal glass tiles) over an animated aurora,
// with an animated 2D spinning UMD logo, an HH:MM clock, a mini "Now Playing"
// PIP and glossy transport. The player carries a translucent Settings overlay
// and unified touch/gesture handling (SDL finger events + mouse-drag parity).
// Renders entirely through Dear ImGui's ImDrawList on top of the SDL2 renderer.
class WmcUi {
public:
    WmcUi();
    ~WmcUi();

    bool init(SDL_Window* window, SDL_Renderer* renderer,
              const std::string& glyph_sample = "");
    void close();

    void process_event(const SDL_Event& e);
    void begin_frame();
    void render();
    void present();

    // --- Hub ---
    // Vertical categories + horizontal tile strip. Mutates selection indices
    // and returns any navigation intent for main() to act on. `loading` speeds
    // up the spinning UMD logo (ISO read in progress). The live video texture
    // feeds the mini "Now Playing" PIP.
    WmcInput render_hub(SDL_Texture* video_tex, int video_w, int video_h,
                        const std::vector<DiscItem>& recent, int& recent_sel,
                        const std::vector<BrowseEntry>& browse, int& browse_sel,
                        const std::string& browse_path,
                        HubMode& category,
                        int& action_sel, bool& extras_open, int& extras_sel,
                        bool& chapters_open, int& chapter_sel,
                        int& setting_sel,
                        const NowPlaying& now_playing, bool loading,
                        bool fullscreen, float volume, bool muted, bool ui_sounds,
                        float t);

    // --- Player ---
    // `settings_open` is toggled by the gear button / O / Tab. `mi` feeds the
    // Settings "Media Info" readout. Playback never stops while the overlay is up.
    WmcInput render_player(SDL_Texture* video_tex, int video_w, int video_h,
                           const DiscItem& disc, const std::string& stream_name,
                           bool paused, double elapsed, double total,
                           bool& osd_visible, bool& settings_open,
                           int& setting_sel, aspect::Mode aspect_mode,
                           const PlayerMediaInfo& mi,
                           float volume, bool muted, bool fullscreen,
                           bool has_menu, bool ui_sounds, bool& menu_clicked,
                           float t);

    // Interactive Disc Menu screen: renders the active .RCO page from `menu`
    // (blitting cached `tex` textures), drives keyboard/gamepad/mouse/touch
    // navigation, and returns the resulting action. `disc` supplies the title
    // for the header bar.
    DiscMenuResult render_disc_menu(umdplay::RcoMenu& menu,
                                    const umdplay::RcoTextureCache& tex,
                                    const DiscItem& disc, float t);

    SDL_Texture* texture_from_png(const std::vector<uint8_t>& bytes,
                                  int* out_w = nullptr, int* out_h = nullptr);

    bool initialized() const { return m_initialized; }

private:
    void draw_aurora(ImDrawList* dl, float w, float h, float t, SDL_Texture* pic1);
    void draw_umd_logo(ImDrawList* dl, float cx, float cy, float r, float t,
                       bool loading);
    void draw_clock(ImDrawList* dl, float w, float h);
    void draw_tile(ImDrawList* dl, const DiscItem& disc, float cx, float cy,
                   float w, float h, float focus, bool selected, float t);
    void draw_browse_tile(ImDrawList* dl, const BrowseEntry& e, float cx, float cy,
                          float w, float h, float focus, bool selected, float t);
    void draw_category_list(ImDrawList* dl, float x, float y, HubMode cat,
                            bool has_now_playing, float t);
    void draw_action_panel(ImDrawList* dl, float x, float y, float w,
                           const DiscItem& disc, int action_sel, bool extras_open,
                           int extras_sel, int bonus_count);
    // Chapters sub-page of the action panel (markers from PLAYLIST.UMD).
    // Drawn as a centred modal over the tile area so a long chapter list fits.
    void draw_chapters_panel(ImDrawList* dl, float w, float h,
                             const std::vector<ChapterItem>& chapters,
                             int chapter_sel);
    void draw_browse_panel(ImDrawList* dl, float x, float y, float w,
                           const std::string& path,
                           const std::vector<BrowseEntry>& entries,
                           int browse_sel);
    void draw_settings_panel(ImDrawList* dl, float x, float y, float w,
                             int setting_sel, bool fullscreen, float volume,
                             bool muted, bool ui_sounds);
    void draw_now_playing_pip(ImDrawList* dl, float w, float h,
                              const NowPlaying& np, SDL_Texture* video_tex,
                              int video_w, int video_h, float t);
    void draw_hub_transport(ImDrawList* dl, float w, float h, bool paused,
                            float t, WmcInput& out);
    void draw_footer(ImDrawList* dl, float w, float h, const std::string& hint);
    // Glossy player transport + timeline. Returns true while the pointer is over
    // the bar (so the caller keeps the OSD pinned open).
    bool draw_osd(ImDrawList* dl, float w, float h, float alpha,
                  bool paused, double elapsed, double total,
                  float volume, bool muted, bool fullscreen,
                  bool settings_open, int chapter, int chapter_count,
                  bool has_menu, bool& gear_clicked, bool& menu_clicked,
                  WmcInput& out);
    // Translucent Settings overlay (does not stop playback).
    void draw_settings_overlay(ImDrawList* dl, float w, float h, float alpha,
                               int setting_sel, aspect::Mode aspect_mode,
                               const PlayerMediaInfo& mi, float volume,
                               bool muted, bool fullscreen, bool ui_sounds,
                               WmcInput& out);

    // Gesture plumbing -------------------------------------------------------
    // Touch and mouse samples are queued (with their own timestamps, both in
    // the SDL_GetTicks base) and drained into the recognizer during render, so
    // the two input paths share one time base.
    struct PointerSample { int kind; float x, y; double t; bool mouse; };  // 0=down 1=move 2=up
    void feed_finger(const SDL_Event& e);   // SDL_FINGER* -> sample queue
    void feed_mouse();                      // mouse-drag parity (desktop)
    void collect_gestures(bool allow_taps, std::vector<gesture::Event>& out);
    void draw_player_huds(ImDrawList* dl, float w, float h, float alpha);

    SDL_Window*   m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    bool          m_initialized = false;
    bool          m_imgui_ready = false;
    SDL_Texture*  m_logo_tex = nullptr;   // official logo (embedded PNG)

    ImFont* m_font_title = nullptr;
    ImFont* m_font_huge  = nullptr;
    ImFont* m_font_body  = nullptr;

    float  m_tile_scroll = 0.0f;    // lerped (rendered) horizontal tile position
    float  m_tile_pos    = 0.0f;    // authoritative carousel position (float index)
    float  m_scroll_vel  = 0.0f;    // inertia velocity after a swipe (indices/s)
    float  m_osd_alpha = 0.0f;
    double m_last_input_time = 0.0;
    bool   m_seeking = false;       // timeline scrub in progress
    float  m_seek_preview = 0.0f;   // scrub position (0..1) while dragging
    HubMode m_last_strip = HubMode::Recent;
    int    m_focus = 0;             // 0 = categories, 1 = tiles, 2 = panel
    float  m_logo_spin = 0.0f;      // UMD logo rotation (radians)

    // Player gesture HUD state.
    gesture::Recognizer m_gesture;
    std::vector<PointerSample> m_ptr_queue;        // pending touch/mouse samples
    std::vector<gesture::Event> m_gesture_queue;   // events awaiting render
    bool   m_touch_active = false;                 // a finger is down
    float  m_scrub_hud = 0.0f;      // >0 while the seek HUD is visible
    double m_scrub_target = 0.0;    // target time shown by the seek HUD
    float  m_vol_hud = 0.0f;        // >0 while the volume HUD is visible
    float  m_dt_hud = 0.0f;         // >0 while the double-tap indicator is up
    float  m_dt_delta = 0.0f;       // -10 or +10 for the double-tap indicator
    bool   m_mouse_gesture = false; // a mouse drag is feeding the recognizer
};
