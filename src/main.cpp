#include "iso_reader.h"
#include <iostream>
#include <iomanip>
#include <string>

static void print_usage(const char* prog) {
    std::cout << "Usage:\n"
              << "  " << prog << " <iso_path>              -- list stream files\n"
              << "  " << prog << " <iso_path> --extract    -- extract .MPS to ./mps_output/\n"
              << "  " << prog << " <iso_path> --dump N     -- hex dump first N bytes of stream N\n";
}

int main(int argc, char* argv[]) {
    std::cout << "\n";
    std::cout << "  ██╗   ██╗███╗   ███╗██████╗ ██╗   ██╗██████╗ ██╗      █████╗ ██╗   ██╗\n";
    std::cout << "  ██║   ██║████╗ ████║██╔══██╗██║   ██║██╔══██╗██║     ██╔══██╗╚██╗ ██╔╝\n";
    std::cout << "  ██║   ██║██╔████╔██║██║  ██║██║   ██║██████╔╝██║     ███████║ ╚████╔╝ \n";
    std::cout << "  ██║   ██║██║╚██╔╝██║██║  ██║╚██╗ ██╔╝██╔═══╝ ██║     ██╔══██║  ╚██╔╝  \n";
    std::cout << "  ╚██████╔╝██║ ╚═╝ ██║██████╔╝ ╚████╔╝ ██║     ███████╗██║  ██║   ██║   \n";
    std::cout << "   ╚═════╝ ╚═╝     ╚═╝╚═════╝   ╚═══╝  ╚═╝     ╚══════╝╚═╝  ╚═╝   ╚═╝  \n";
    std::cout << "  UMD Video Player — Phase 1 ISO Parser  |  M5Dev  |  GPL v2\n\n";

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string iso_path = argv[1];
    std::string mode     = (argc >= 3) ? argv[2] : "";

    IsoReader reader(iso_path);
    if (!reader.open()) {
        std::cerr << "\n❌ Failed to parse ISO.\n";
        return 1;
    }

    auto entries = reader.get_mps_entries();

    // ── List mode (default) ──────────────────────────────────────────────────
    std::cout << "\n📋 Stream Files in UMD_VIDEO/STREAM/:\n";
    std::cout << std::left
              << std::setw(4)  << "#"
              << std::setw(16) << "Name"
              << std::setw(10) << "LBA"
              << std::setw(16) << "Offset (hex)"
              << "~Size\n";
    std::cout << std::string(58, '-') << "\n";

    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        std::cout << std::setw(4)  << i
                  << std::setw(16) << e.name
                  << std::setw(10) << e.lba
                  << "0x" << std::hex << std::setw(10) << std::setfill('0')
                  << (static_cast<uint64_t>(e.lba) * UMD_SECTOR_SIZE)
                  << std::dec << std::setfill(' ')
                  << "  ~" << e.size_bytes / 1048576 << " MB\n";
    }

    // ── Extract mode ─────────────────────────────────────────────────────────
    if (mode == "--extract") {
        std::string out_dir = "mps_output";
        std::cout << "\n💾 Extracting to ./" << out_dir << "/\n";
        // Note: create the directory before running (mkdir mps_output)
        reader.extract_all(out_dir);
    }

    // ── Dump mode ─────────────────────────────────────────────────────────────
    if (mode == "--dump" && argc >= 4) {
        int idx = std::stoi(argv[3]);
        if (idx >= 0 && static_cast<size_t>(idx) < entries.size()) {
            reader.dump_mps_header(entries[idx], 128);
        } else {
            std::cerr << "❌ Invalid stream index: " << idx << "\n";
        }
    }

    // ── Always dump first entry header for verification ───────────────────────
    if (mode.empty() && !entries.empty()) {
        reader.dump_mps_header(entries[0], 64);
    }

    std::cout << "\n✅ Phase 1 complete — ready for Phase 2 (FFmpeg decoder)\n\n";
    return 0;
}
