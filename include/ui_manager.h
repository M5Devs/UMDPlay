#pragma once

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
}

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

    void render_video_frame(AVFrame* frame);
    void queue_audio(const uint8_t* data, uint32_t len);

    UserAction handle_events();

    bool is_initialized() const { return m_initialized; }

private:
    bool m_initialized = false;
    SDL_Window* m_window = nullptr;
    SDL_Renderer* m_renderer = nullptr;
    SDL_Texture* m_texture = nullptr;
    SDL_AudioDeviceID m_audio_dev = 0;

    int m_texture_width = 0;
    int m_texture_height = 0;

    void update_texture_size(int width, int height);
};
