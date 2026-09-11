// .wav reader for the audio SDK. No platform code.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Wav {
    std::vector<uint8_t> format;  // the "fmt " chunk (WAVEFORMATEX layout, padded to at least 18 bytes)
    std::vector<uint8_t> data;    // the samples
};

// Throws std::runtime_error when the bytes aren't a RIFF/WAVE file with fmt and data chunks.
Wav parseWav(const std::string& bytes);
