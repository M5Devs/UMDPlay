#include "iso_reader.h"
#include "decoder.h"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: umdplay <path_to_umd_video.iso>\n";
        return 1;
    }

    std::string iso_path = argv[1];
    std::cout << "=== UMDPlay Preservation Engine ===\n";
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
    std::cout << "[*] Decoding first frames...\n";
    if (decoder.test_decode(30, "output_frame.ppm")) {
        std::cout << "✅ Frame decoded successfully to: output_frame.ppm\n";
    }

    return 0;
}
