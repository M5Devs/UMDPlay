#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

struct ImFont;
struct ImDrawList;
struct ImVec2;

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
};

// Navigation intents emitted by the UI, consumed by main().
enum class WmcAction {
    None,
    Quit,
    Play,       // Enter on a stream
    BackToHub,  // Esc in player mode
    TogglePause,
    SeekBackward,
    SeekForward,
    SeekTo      // seek bar drag (absolute seconds in `seek_seconds`)
};

struct WmcInput {
    WmcAction action = WmcAction::None;
    double    seek_seconds = 0.0;
};

// Windows Media Center inspired 10-foot interface: animated cobalt gradient,
// glass card carousel and a translucent transport OSD. Renders entirely through
// Dear ImGui's ImDrawList on top of the shared SDL2 renderer.
class WmcUi {
public:
    WmcUi();
    ~WmcUi();

    bool init(SDL_Window* window, SDL_Renderer* renderer,
              const std::string& glyph_sample = "");
    void close();

    // Feed SDL events so ImGui (and our nav layer) can react.
    void process_event(const SDL_Event& e);

    void begin_frame();
    void render();     // ImGui::Render + draw data into the renderer (no present)
    void present();    // SDL_RenderPresent

    // --- Hub (carousel) ---
    // Returns navigation intent for the hub. `t` is seconds since start.
    WmcInput render_hub(const std::vector<DiscItem>& discs,
                        int& selected_disc, int& selected_stream, float t);

    // --- Player (video + OSD) ---
    // `video_tex` is the streaming IYUV texture; pass nullptr for audio-only.
    WmcInput render_player(SDL_Texture* video_tex, int video_w, int video_h,
                           const DiscItem& disc, const std::string& stream_name,
                           bool paused, double elapsed, double total,
                           bool& osd_visible, float t);

    // Decode PNG bytes into an SDL_Texture (RGBA32). Returns nullptr on failure.
    SDL_Texture* texture_from_png(const std::vector<uint8_t>& bytes,
                                  int* out_w = nullptr, int* out_h = nullptr);

    // Scale factor applied to all 10-foot typography (1.0 = 720p baseline).
    void set_ui_scale(float s) { m_ui_scale = s; }

    bool initialized() const { return m_initialized; }

private:
    void draw_background(ImDrawList* dl, float w, float h, float t, SDL_Texture* pic1);
    void draw_card(ImDrawList* dl, const DiscItem& disc, float cx, float cy,
                   float w, float h, float focus, bool selected, float t);
    void draw_osd(ImDrawList* dl, float w, float h, float alpha,
                  bool paused, double elapsed, double total, WmcInput& out);

    SDL_Window*   m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    bool          m_initialized = false;
    bool          m_imgui_ready = false;
    float         m_ui_scale = 1.0f;

    ImFont* m_font_title = nullptr;
    ImFont* m_font_huge  = nullptr;
    ImFont* m_font_body  = nullptr;

    // Smooth carousel scroll position (lerped toward selected index).
    float m_scroll = 0.0f;
    float m_osd_alpha = 0.0f;
    double m_last_input_time = 0.0;
};
