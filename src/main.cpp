#include "iso_reader.h"
#include "decoder.h"
#include <iostream>

int main(int argc, char** argv) {
    std::string iso_path = "/content/miku_umd.iso";
    if (argc > 1) iso_path = argv[1];

    IsoReader reader(iso_path);
    if (!reader.open()) return 1;

    auto entries = reader.get_mps_entries();
    const MpsEntry* main_feature = nullptr;
    for (const auto& e : entries) {
        if (!main_feature || e.size_bytes > main_feature->size_bytes) {
            main_feature = &e;
        }
    }

    std::cout << "[*] Main Feature: " << main_feature->name 
              << " (" << main_feature->size_bytes / (1024*1024) << " MB)" << std::endl;

    Decoder decoder;
    if (!decoder.open_stream(iso_path, *main_feature)) return 1;

    // هننط لـ 500 ميجابايت في قلب الحفلة!
    std::cout << "[*] 🚀 Jumping to ~500 MB (Right into the middle of the concert!)..." << std::endl;
    decoder.test_decode(120);

    return 0;
}
