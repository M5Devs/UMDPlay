#include "iso_reader.h"
#include "decoder.h"
#include "ui_manager.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: umdplay <path_to_umd_video.iso>\n";
        return 1;
    }

    std::string iso_path = argv[1];
    std::cout << "=== UMDPlay Preservation Engine (Phase 3) ===\n";
    std::cout << "[*] Inspecting ISO: " << iso_path << "\n";

    IsoReader reader(iso_path);
    if (!reader.open()) {
        std::cerr << "[-] Failed to read UMD Video filesystem!\n";
        return 1;
    }

    auto entries = reader.get_mps_entries();
    if (entries.empty()) {
        std::cerr << "[-] No .MPS streams found!\n";
        return 1;
    }

    const MpsEntry* main_feature = nullptr;
    for (const auto& e : entries) {
        std::cout << "    - " << e.name << " @ LBA " << e.lba 
                  << " (" << e.size_bytes / (1024 * 1024) << " MB)\n";
        if (!main_feature || e.size_bytes > main_feature->size_bytes) {
            main_feature = &e;
        }
    }

    std::cout << "\n[*] Streaming Main Feature: " << main_feature->name << "\n";
    Decoder decoder;
    if (!decoder.open_stream(iso_path, *main_feature)) {
        std::cerr << "[-] Failed to probe stream!\n";
        return 1;
    }

    std::cout << "[+] Stream successfully opened in memory!\n";

    UiManager ui;
    std::string title = "UMDPlay - " + main_feature->name;
    if (!ui.init(title, 960, 544)) {
        std::cerr << "[-] Failed to initialize UI window!\n";
        return 1;
    }

    double fps = decoder.get_fps();
    if (fps <= 0.0 || fps > 120.0) fps = 29.97;
    uint32_t frame_delay_ms = static_cast<uint32_t>(1000.0 / fps);
    std::cout << "[*] Stream FPS: " << fps << " (delay: " << frame_delay_ms << " ms per frame)\n";

    bool running = true;
    bool paused = false;

    AVFrame* video_frame = av_frame_alloc();
    std::vector<uint8_t> audio_pcm;

    uint32_t last_frame_time = SDL_GetTicks();

    while (running) {
        // Process UI / User input events
        UserAction action = ui.handle_events();
        switch (action) {
            case UserAction::Quit:
                running = false;
                break;
            case UserAction::TogglePause:
                paused = !paused;
                std::cout << (paused ? "[*] Playback Paused\n" : "[*] Playback Resumed\n");
                break;
            case UserAction::SeekBackward:
                std::cout << "[*] Seeking backward 5s...\n";
                decoder.seek_relative(-5.0);
                break;
            case UserAction::SeekForward:
                std::cout << "[*] Seeking forward 5s...\n";
                decoder.seek_relative(5.0);
                break;
            case UserAction::None:
            default:
                break;
        }

        if (!running) break;

        if (!paused) {
            FrameType ft = decoder.read_frame(video_frame, audio_pcm);

            if (ft == FrameType::Eof) {
                std::cout << "[*] Reached End of File (EOF).\n";
                break;
            } else if (ft == FrameType::Video) {
                // Render video frame
                ui.render_video_frame(video_frame);

                // Real-time FPS throttle
                uint32_t current_time = SDL_GetTicks();
                uint32_t elapsed = current_time - last_frame_time;
                if (elapsed < frame_delay_ms) {
                    SDL_Delay(frame_delay_ms - elapsed);
                }
                last_frame_time = SDL_GetTicks();
            } else if (ft == FrameType::Audio) {
                if (!audio_pcm.empty()) {
                    ui.queue_audio(audio_pcm.data(), static_cast<uint32_t>(audio_pcm.size()));
                }
            }
        } else {
            SDL_Delay(20);
        }
    }

    av_frame_free(&video_frame);
    ui.close();
    decoder.close();

    std::cout << "[+] Playback completed cleanly.\n";
    return 0;
}
