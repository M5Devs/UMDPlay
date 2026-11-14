#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;

// One playable stream inside a disc (an .MPS file).
struct StreamItem {
    std::string name;
    uint32_t    size_bytes = 0;
    bool        is_main    = false;
};

// A disc (one ISO) shown as a carousel card.
struct DiscItem {
    std::string              iso_path;
    std::string              title;      // PARAM.SFO TITLE
    std::string              disc_id;    // PARAM.SFO DISC_ID
    std::string              category;   // PARAM.SFO CATEGORY (UV / UA / UG)
    std::vector<StreamItem>  streams;

    SDL_Texture*             icon = nullptr;   // ICON0.PNG
    SDL_Texture*             pic1 = nullptr;   // PIC1.PNG (optional)

    bool is_demo = false;                      // synthetic entry (--demo)
};

// What the hub is currently showing.
enum class HubMode {
    Recent,       // the Recent Discs strip
    Browse,       // the Browse System (folder/file) strip
    Settings      // the Options strip
};

// One entry in the Browse System listing (a folder or a .iso file).
struct BrowseEntry {
    std::string name;
    std::string path;
    bool        is_dir = false;
    bool        is_iso = false;
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
    TogglePause,
    SeekBackward,
    SeekForward,
    SeekRelative,       // seek by `seek_delta` seconds (negative = back)
    Stop,               // stop playback, return to the Hub
    SeekTo,             // seek bar drag (absolute seconds in `seek_seconds`)
    ToggleFullscreen,
    VolumeChanged,      // new gain in `volume`
    MuteToggled
};

struct WmcInput {
    WmcAction action = WmcAction::None;
    double    seek_seconds = 0.0;
    double    seek_delta = 0.0;
    int       stream_index = -1;
    float     volume = 1.0f;
    std::string iso_path;
};

// Windows Media Center inspired 10-foot interface: an animated cobalt backdrop
// with horizontal strips (Recent / Browse / Options), a disc carousel with
// clean Play / Extras / Options actions, and a translucent player transport OSD.
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
    // `recent` and `browse` are the two card rows; `browse_path` is the folder
    // currently shown in the Browse strip. Mutates selection indices and
    // returns any navigation intent for main() to act on.
    WmcInput render_hub(HubMode& strip,
                        const std::vector<DiscItem>& recent, int& recent_sel,
                        const std::vector<BrowseEntry>& browse, int& browse_sel,
                        const std::string& browse_path,
                        int& action_sel, bool& extras_open, int& extras_sel,
                        int& setting_sel,
                        bool fullscreen, float volume, bool muted, float t);

    // --- Player ---
    WmcInput render_player(SDL_Texture* video_tex, int video_w, int video_h,
                           const DiscItem& disc, const std::string& stream_name,
                           bool paused, double elapsed, double total,
                           bool& osd_visible, float volume, bool muted,
                           bool fullscreen, float t);

    SDL_Texture* texture_from_png(const std::vector<uint8_t>& bytes,
                                  int* out_w = nullptr, int* out_h = nullptr);

    bool initialized() const { return m_initialized; }

private:
    void draw_background(ImDrawList* dl, float w, float h, float t, SDL_Texture* pic1);
    void draw_card(ImDrawList* dl, const DiscItem& disc, float cx, float cy,
                   float w, float h, float focus, bool selected, float t);
    void draw_strip_tabs(ImDrawList* dl, float w, float y, HubMode strip, float t);
    void draw_action_panel(ImDrawList* dl, float x, float y, float w,
                           const DiscItem& disc, int action_sel, bool extras_open,
                           int extras_sel, int bonus_count);
    void draw_browse_panel(ImDrawList* dl, float x, float y, float w,
                           const std::string& path,
                           const std::vector<BrowseEntry>& entries,
                           int browse_sel, float scroll);
    void draw_settings_panel(ImDrawList* dl, float x, float y, float w,
                             int setting_sel, bool fullscreen, float volume,
                             bool muted);
    void draw_footer(ImDrawList* dl, float w, float h, const std::string& hint);
    // Returns true when the pointer is over the transport bar (so the caller can
    // keep the OSD pinned open).
    bool draw_osd(ImDrawList* dl, float w, float h, float alpha,
                  bool paused, double elapsed, double total,
                  float volume, bool muted, bool fullscreen, WmcInput& out);

    SDL_Window*   m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    bool          m_initialized = false;
    bool          m_imgui_ready = false;

    ImFont* m_font_title = nullptr;
    ImFont* m_font_huge  = nullptr;
    ImFont* m_font_body  = nullptr;

    float  m_scroll = 0.0f;         // lerped carousel position
    float  m_osd_alpha = 0.0f;
    double m_last_input_time = 0.0;
    bool   m_seeking = false;       // timeline scrub in progress
    float  m_seek_preview = 0.0f;   // scrub position (0..1) while dragging
    HubMode m_last_strip = HubMode::Recent;
    int    m_focus = 0;             // 0 = card row, 1 = action row
    float  m_browse_scroll = 0.0f;
    int    m_browse_cursor = 0;     // cursor row inside the file list
};
