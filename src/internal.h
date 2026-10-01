#pragma once
#include <aardvark_audio/audio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

namespace aardvark::audio::detail {

struct Wave {
    std::size_t data = 0;
    std::size_t bytes = 0;
    std::uint16_t tag = 0;
    std::uint16_t bits = 0;
    std::uint16_t valid_bits = 0;
    std::uint16_t align = 0;
    std::uint16_t block_frames = 0;
    bool big = false;
    std::vector<std::array<std::int16_t, 2>> coefficients;
};

std::uint16_t u16(const std::uint8_t* p, bool big = false) noexcept;
std::uint32_t u32(const std::uint8_t* p, bool big = false) noexcept;
std::uint64_t u64(const std::uint8_t* p, bool big = false) noexcept;
Error parse_wave(const std::vector<std::uint8_t>& bytes, Limits limits, Format& format, Wave& wave);
float wave_sample(const std::uint8_t* sample, const Wave& wave) noexcept;
Error wave_block(const std::uint8_t* block, const Wave& wave, unsigned channels, std::vector<std::int16_t>& pcm);
Error decode_mp3(const std::vector<std::uint8_t>& bytes, Limits limits, Format& format, std::vector<float>& pcm);

inline std::int16_t to_s16(float sample) noexcept {
    if (std::isnan(sample)) return 0;
    if (sample >= 1.0f) return 32767;
    if (sample <= -1.0f) return -32768;
    return static_cast<std::int16_t>(std::clamp(std::round(sample * 32768.0f), -32768.0f, 32767.0f));
}

inline std::int16_t clamp16(std::int64_t sample) noexcept {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(sample, -32768, 32767));
}

}
