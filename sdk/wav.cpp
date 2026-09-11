#include "wav.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

Wav parseWav(const std::string& f) {
    if (f.size() < 12 || f.compare(0, 4, "RIFF") != 0 || f.compare(8, 4, "WAVE") != 0)
        throw std::runtime_error("não é um arquivo WAV");
    Wav w;
    for (size_t i = 12; i + 8 <= f.size();) {
        uint32_t len;
        memcpy(&len, &f[i + 4], 4);
        size_t n = std::min<size_t>(len, f.size() - i - 8);  // truncated file: keep what is there
        auto body = reinterpret_cast<const uint8_t*>(f.data() + i + 8);
        if (f.compare(i, 4, "fmt ") == 0) w.format.assign(body, body + n);
        else if (f.compare(i, 4, "data") == 0) w.data.assign(body, body + n);
        i += 8 + size_t(len) + (len & 1);  // chunks are padded to even sizes
    }
    if (w.format.size() < 16 || w.data.empty()) throw std::runtime_error("WAV sem os blocos fmt/data");
    w.format.resize(std::max<size_t>(w.format.size(), 18));  // PCM's 16-byte fmt has no cbSize: pad it with 0
    return w;
}
