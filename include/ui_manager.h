#pragma once

#include "video_frame.h"

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

enum class UserAction {
    None,
    Quit,
    TogglePause,
    SeekBackward,
    SeekForward
};

class UiManager {
public:
    UiManager();
    ~UiManager();

    bool init(const std::string& title = "UMDPlay - PSP UMD Video Player", int width = 960, int height = 544);
    void close();

    // Set the taskbar/window icon from the official logo embedded in the binary
    // (assets/umdplay_logo.png via include/umdplay_logo.h). No-op if the window
    // is missing or the PNG fails to decode.
    void set_window_icon();

    // Upload a decoded frame into the streaming video texture (no present).
    void update_video_texture(const umdplay::VideoFrame& frame);
    // Upload a synthetic planar YUV420P image (Y then U then V), for demos.
    void update_demo_texture(const uint8_t* yuv, int width, int height);
    void queue_audio(const uint8_t* data, uint32_t len);

    // Pause/resume the SDL audio device so queued audio holds its place in the
    // buffer instead of draining while video is paused.
    void pause_audio(bool paused);
    // Drop everything still queued (called on seek/stop so no stale audio plays).
    void clear_audio_queue();
    // Bytes still waiting in the SDL queue (0 when the device is unavailable).
    uint32_t queued_audio_bytes() const;

    // Window display mode. toggle_fullscreen() flips between a windowed frame
    // and SDL_WINDOW_FULLSCREEN_DESKTOP and returns the new fullscreen state.
    bool toggle_fullscreen();
    void set_fullscreen(bool on);
    bool is_fullscreen() const { return m_fullscreen; }

    // Playback gain applied in queue_audio(). volume is clamped to [0, 1].
    void  set_volume(float v);
    float volume() const { return m_volume; }
    void  set_muted(bool m) { m_muted = m; }
    bool  muted() const { return m_muted; }

    // Legacy single-shot present used by simple callers.
    void render_video_frame(const umdplay::VideoFrame& frame);

    UserAction handle_events();

    bool is_initialized() const { return m_initialized; }

    SDL_Window*   window()   const { return m_window; }
    SDL_Renderer* renderer() const { return m_renderer; }
    SDL_Texture*  video_texture() const { return m_texture; }
    int           video_width()  const { return m_texture_width; }
    int           video_height() const { return m_texture_height; }
    void          set_window_size(int w, int h);

private:
    bool m_initialized = false;
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    SDL_AudioDeviceID m_audio_dev = 0;

    int m_texture_width = 0;
    int m_texture_height = 0;

    bool  m_fullscreen = false;
    float m_volume = 1.0f;
    bool  m_muted = false;
    std::vector<uint8_t> m_audio_scratch;

    void update_texture_size(int width, int height);
};
