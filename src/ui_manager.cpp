#include "ui_manager.h"
#include <iostream>

UiManager::UiManager() {}

UiManager::~UiManager() {
    close();
}

bool UiManager::init(const std::string& title, int width, int height) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        std::cerr << "[-] SDL_Init failed: " << SDL_GetError() << "\n";
        return false;
    }

    m_window = SDL_CreateWindow(
        title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        width,
        height,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    if (!m_window) {
        std::cerr << "[-] SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        return false;
    }

    m_renderer = SDL_CreateRenderer(
        m_window,
        -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );

    if (!m_renderer) {
        // Fallback to software renderer if accelerated is not supported (e.g. in dummy/headless driver)
        m_renderer = SDL_CreateRenderer(m_window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (!m_renderer) {
        std::cerr << "[-] SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        return false;
    }

    // Audio setup
    SDL_AudioSpec wanted_spec, obtained_spec;
    SDL_zero(wanted_spec);
    wanted_spec.freq = 44100;
    wanted_spec.format = AUDIO_S16SYS;
    wanted_spec.channels = 2;
    wanted_spec.samples = 1024;
    wanted_spec.callback = nullptr; // Using SDL_QueueAudio

    m_audio_dev = SDL_OpenAudioDevice(nullptr, 0, &wanted_spec, &obtained_spec, 0);
    if (m_audio_dev == 0) {
        std::cerr << "[-] SDL_OpenAudioDevice failed: " << SDL_GetError() << "\n";
    } else {
        SDL_PauseAudioDevice(m_audio_dev, 0); // Start playback
    }

    m_initialized = true;
    return true;
}

void UiManager::update_texture_size(int width, int height) {
    if (m_texture && (m_texture_width != width || m_texture_height != height)) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }

    if (!m_texture && width > 0 && height > 0) {
        m_texture_width = width;
        m_texture_height = height;
        m_texture = SDL_CreateTexture(
            m_renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            width,
            height
        );
        if (!m_texture) {
            std::cerr << "[-] SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        }
    }
}

void UiManager::update_video_texture(AVFrame* frame) {
    if (!m_renderer || !frame) return;
    update_texture_size(frame->width, frame->height);
    if (m_texture) {
        SDL_UpdateYUVTexture(
            m_texture,
            nullptr,
            frame->data[0], frame->linesize[0],
            frame->data[1], frame->linesize[1],
            frame->data[2], frame->linesize[2]
        );
    }
}

void UiManager::update_demo_texture(const uint8_t* yuv, int width, int height) {
    if (!m_renderer || !yuv || width <= 0 || height <= 0) return;
    update_texture_size(width, height);
    if (!m_texture) return;
    const int cw = width / 2, ch = height / 2;
    SDL_UpdateYUVTexture(m_texture, nullptr,
                         yuv, width,
                         yuv + static_cast<size_t>(width) * height, cw,
                         yuv + static_cast<size_t>(width) * height + cw * ch, cw);
}

void UiManager::render_video_frame(AVFrame* frame) {
    if (!m_renderer || !frame) return;
    update_video_texture(frame);
    if (m_texture) {
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_texture, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);
    }
}

void UiManager::set_window_size(int w, int h) {
    if (m_window) SDL_SetWindowSize(m_window, w, h);
}

void UiManager::set_fullscreen(bool on) {
    if (!m_window) return;
    if (on == m_fullscreen) return;
    if (SDL_SetWindowFullscreen(m_window,
            on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0) {
        m_fullscreen = on;
    }
}

bool UiManager::toggle_fullscreen() {
    set_fullscreen(!m_fullscreen);
    return m_fullscreen;
}

void UiManager::set_volume(float v) {
    m_volume = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void UiManager::queue_audio(const uint8_t* data, uint32_t len) {
    if (m_audio_dev == 0 || !data || len == 0) return;

    if (m_muted || m_volume <= 0.0f) return;
    if (m_volume >= 1.0f) {
        SDL_QueueAudio(m_audio_dev, data, len);
        return;
    }

    // Scale S16 stereo samples in place into a scratch buffer.
    const size_t samples = len / sizeof(int16_t);
    m_audio_scratch.resize(len);
    const int16_t* src = reinterpret_cast<const int16_t*>(data);
    int16_t* dst = reinterpret_cast<int16_t*>(m_audio_scratch.data());
    for (size_t i = 0; i < samples; ++i) {
        dst[i] = static_cast<int16_t>(src[i] * m_volume);
    }
    SDL_QueueAudio(m_audio_dev, m_audio_scratch.data(), len);
}

void UiManager::pause_audio(bool paused) {
    if (m_audio_dev == 0) return;
    SDL_PauseAudioDevice(m_audio_dev, paused ? 1 : 0);
}

void UiManager::clear_audio_queue() {
    if (m_audio_dev == 0) return;
    SDL_ClearQueuedAudio(m_audio_dev);
}

uint32_t UiManager::queued_audio_bytes() const {
    if (m_audio_dev == 0) return 0;
    return SDL_GetQueuedAudioSize(m_audio_dev);
}

UserAction UiManager::handle_events() {
    SDL_Event event;
    UserAction action = UserAction::None;

    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            action = UserAction::Quit;
        } else if (event.type == SDL_KEYDOWN) {
            switch (event.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:
                    action = UserAction::Quit;
                    break;
                case SDLK_SPACE:
                    action = UserAction::TogglePause;
                    break;
                case SDLK_LEFT:
                    action = UserAction::SeekBackward;
                    break;
                case SDLK_RIGHT:
                    action = UserAction::SeekForward;
                    break;
                default:
                    break;
            }
        }
    }

    return action;
}

void UiManager::close() {
    if (m_audio_dev != 0) {
        SDL_CloseAudioDevice(m_audio_dev);
        m_audio_dev = 0;
    }
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    if (m_renderer) {
        SDL_DestroyRenderer(m_renderer);
        m_renderer = nullptr;
    }
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_initialized) {
        SDL_Quit();
        m_initialized = false;
    }
}
