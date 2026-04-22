// Minimal WAV file writer for diagnostic audio dump during tests.
//
// Used to render PCM snapshots to .wav for manual ear-inspection when a
// test output looks suspicious. Not performance-critical; writes a single
// channel int16 PCM WAV with a standard 44-byte RIFF header.

#ifndef TGSB_TEST_WAV_EXPORT_H
#define TGSB_TEST_WAV_EXPORT_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace tgsb_test {

inline bool writeWav(const std::string& path,
                     const std::vector<std::int16_t>& pcm,
                     int sampleRate) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const std::uint32_t dataSize =
        static_cast<std::uint32_t>(pcm.size() * sizeof(std::int16_t));
    const std::uint32_t chunkSize = 36 + dataSize;
    const std::uint16_t audioFormat = 1;  // PCM
    const std::uint16_t numChannels = 1;
    const std::uint32_t sr = static_cast<std::uint32_t>(sampleRate);
    const std::uint16_t bitsPerSample = 16;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(numChannels * bitsPerSample / 8);
    const std::uint32_t byteRate = sr * blockAlign;
    const std::uint16_t fmtSize = 16;

    auto writeU32 = [&](std::uint32_t v) {
        std::uint8_t buf[4] = {
            static_cast<std::uint8_t>(v & 0xff),
            static_cast<std::uint8_t>((v >> 8) & 0xff),
            static_cast<std::uint8_t>((v >> 16) & 0xff),
            static_cast<std::uint8_t>((v >> 24) & 0xff)
        };
        std::fwrite(buf, 1, 4, f);
    };
    auto writeU16 = [&](std::uint16_t v) {
        std::uint8_t buf[2] = {
            static_cast<std::uint8_t>(v & 0xff),
            static_cast<std::uint8_t>((v >> 8) & 0xff)
        };
        std::fwrite(buf, 1, 2, f);
    };

    std::fwrite("RIFF", 1, 4, f);
    writeU32(chunkSize);
    std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f);
    writeU32(fmtSize);
    writeU16(audioFormat);
    writeU16(numChannels);
    writeU32(sr);
    writeU32(byteRate);
    writeU16(blockAlign);
    writeU16(bitsPerSample);
    std::fwrite("data", 1, 4, f);
    writeU32(dataSize);

    if (!pcm.empty()) {
        std::fwrite(pcm.data(), sizeof(std::int16_t), pcm.size(), f);
    }
    std::fclose(f);
    return true;
}

}  // namespace tgsb_test

#endif  // TGSB_TEST_WAV_EXPORT_H
