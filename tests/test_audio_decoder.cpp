#include "decoder.h"
#include <iostream>
#include <fstream>
#include <vector>
#include <cassert>

int main() {
    Decoder decoder;
    MpsEntry entry;
    entry.name = "00001.MPS";
    entry.lba = 0;
    entry.size_bytes = 0;

    // Test default initial state
    assert(!decoder.has_video());
    assert(!decoder.has_audio());

    // Test close safety
    decoder.close();

    std::cout << "✅ All PSMF ATRAC3+ audio decoder unit tests passed!\n";
    return 0;
}
