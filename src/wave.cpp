#include "internal.h"

namespace aardvark::audio::detail {

std::uint16_t u16(const std::uint8_t* p, bool big) noexcept {
    return static_cast<std::uint16_t>(big ? (unsigned(p[0]) << 8) | p[1] : (unsigned(p[1]) << 8) | p[0]);
}

std::uint32_t u32(const std::uint8_t* p, bool big) noexcept {
    return big ? (std::uint32_t(u16(p, true)) << 16) | u16(p + 2, true) : (std::uint32_t(u16(p + 2)) << 16) | u16(p);
}

std::uint64_t u64(const std::uint8_t* p, bool big) noexcept {
    return big ? (std::uint64_t(u32(p, true)) << 32) | u32(p + 4, true) : (std::uint64_t(u32(p + 4)) << 32) | u32(p);
}

static bool tag(const std::uint8_t* p, const char* name) noexcept { return std::memcmp(p, name, 4) == 0; }

static Error parse_format(const std::uint8_t* p, std::size_t size, Limits limits, Format& format, Wave& wave) {
    if (size < 16 || size == 17) return Error::invalid_data;
    wave.tag = u16(p, wave.big);
    format.channels = u16(p + 2, wave.big);
    format.sample_rate = u32(p + 4, wave.big);
    wave.align = u16(p + 12, wave.big);
    wave.bits = u16(p + 14, wave.big);
    wave.valid_bits = wave.bits;
    if (!format.channels || !format.sample_rate || !wave.align) return Error::invalid_data;
    if (format.channels > limits.channels || format.sample_rate > limits.sample_rate) return Error::limit_exceeded;
    if (size >= 18 && u16(p + 16, wave.big) > size - 18) return Error::invalid_data;
    if (wave.tag == 0xfffe) {
        if (size < 40 || u16(p + 16, wave.big) < 22) return Error::invalid_data;
        if (wave.big) return Error::unsupported;
        constexpr std::uint8_t suffix[] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71};
        if (std::memcmp(p + 26, suffix, sizeof(suffix))) return Error::unsupported;
        wave.tag = u16(p + 24);
        wave.valid_bits = u16(p + 18);
        format.channel_mask = u32(p + 20);
        auto mask = format.channel_mask;
        unsigned count = 0;
        while (mask) { count += mask & 1; mask >>= 1; }
        if (format.channel_mask && count != format.channels) return Error::invalid_data;
        if (!wave.valid_bits) wave.valid_bits = wave.bits;
        if (wave.valid_bits > wave.bits) return Error::invalid_data;
    }
    if (wave.tag == 1 || wave.tag == 3 || wave.tag == 6 || wave.tag == 7) {
        const bool supported = wave.tag == 1 ? wave.bits == 8 || wave.bits == 16 || wave.bits == 24 || wave.bits == 32 :
            wave.tag == 3 ? (wave.bits == 32 || wave.bits == 64) && wave.valid_bits == wave.bits : wave.bits == 8;
        if (!supported) return Error::unsupported;
        if (wave.align != format.channels * (wave.bits / 8)) return Error::invalid_data;
        return Error::none;
    }
    if (wave.tag != 2 && wave.tag != 17) return Error::unsupported;
    if (wave.big || format.channels > 2 || wave.bits != 4) return Error::unsupported;
    if (size < 20 || u16(p + 16) < 2) return Error::invalid_data;
    wave.block_frames = u16(p + 18);
    if (wave.tag == 17) {
        const unsigned prefix = 4 * format.channels;
        if (wave.align < prefix || (wave.align - prefix) % (4 * format.channels) ||
            wave.block_frames != 1 + (wave.align - prefix) * 2 / format.channels) return Error::invalid_data;
    } else {
        const unsigned prefix = 7 * format.channels;
        if (size < 22 || u16(p + 16) < 4 || wave.align < prefix ||
            wave.block_frames != 2 + (wave.align - prefix) * 2 / format.channels) return Error::invalid_data;
        const unsigned count = u16(p + 20);
        if (!count || count > 256 || size < 22 + count * 4 || u16(p + 16) < 4 + count * 4) return Error::invalid_data;
        for (unsigned i = 0; i < count; ++i) {
            wave.coefficients.push_back({static_cast<std::int16_t>(u16(p + 22 + i * 4)), static_cast<std::int16_t>(u16(p + 24 + i * 4))});
        }
    }
    return Error::none;
}

Error parse_wave(const std::vector<std::uint8_t>& bytes, Limits limits, Format& format, Wave& wave) {
    const auto* p = bytes.data();
    if (bytes.size() < 12 || !tag(p + 8, "WAVE")) return Error::invalid_data;
    wave.big = tag(p, "RIFX");
    const bool extended = tag(p, "RF64");
    std::uint64_t end = std::uint64_t(u32(p + 4, wave.big)) + 8;
    std::uint64_t data_size = 0;
    std::uint64_t sample_count = 0;
    bool have_format = false, have_data = false, have_fact = false;
    std::uint64_t fact = 0;
    std::size_t offset = 12;
    if (extended) {
        if (u32(p + 4) != UINT32_MAX || bytes.size() < 48 || !tag(p + 12, "ds64")) return Error::invalid_data;
        const auto size = u32(p + 16);
        if (size < 28 || size > bytes.size() - 20) return Error::invalid_data;
        const auto riff = u64(p + 20);
        if (riff > UINT64_MAX - 8) return Error::invalid_data;
        end = riff + 8;
        data_size = u64(p + 28);
        sample_count = u64(p + 36);
        if (u32(p + 44)) return Error::unsupported;
        offset = 20 + std::size_t(size) + (size & 1);
    }
    if (end > bytes.size() || end < offset) return Error::invalid_data;
    unsigned chunks = 0;
    while (offset < end) {
        if (++chunks > 65536) return Error::limit_exceeded;
        if (end - offset < 8) return Error::invalid_data;
        const auto* chunk = p + offset;
        std::uint64_t size = u32(chunk + 4, wave.big);
        if (extended && tag(chunk, "data") && size == UINT32_MAX) size = data_size;
        offset += 8;
        if (size > end - offset || (size & 1) > end - offset - size) return Error::invalid_data;
        if (tag(chunk, "fmt ")) {
            if (have_format) return Error::invalid_data;
            const auto result = parse_format(p + offset, static_cast<std::size_t>(size), limits, format, wave);
            if (result != Error::none) return result;
            have_format = true;
        } else if (tag(chunk, "data")) {
            if (have_data) return Error::invalid_data;
            wave.data = offset;
            wave.bytes = static_cast<std::size_t>(size);
            have_data = true;
        } else if (tag(chunk, "fact")) {
            if (have_fact || size < 4) return Error::invalid_data;
            fact = u32(p + offset, wave.big);
            have_fact = true;
        }
        offset += static_cast<std::size_t>(size) + static_cast<std::size_t>(size & 1);
    }
    if (!have_format || !have_data || wave.bytes % wave.align) return Error::invalid_data;
    format.frames = wave.bytes / wave.align;
    if (wave.block_frames) {
        format.frames *= wave.block_frames;
        if (have_fact) {
            if (fact > format.frames || (format.frames && (!fact || format.frames - fact >= wave.block_frames))) return Error::invalid_data;
            format.frames = fact;
        }
    }
    if (extended && sample_count && sample_count != format.frames) return Error::invalid_data;
    if (format.frames > limits.decoded_bytes / sizeof(float) / format.channels) return Error::limit_exceeded;
    format.codec = Codec::wave;
    return Error::none;
}

float wave_sample(const std::uint8_t* p, const Wave& wave) noexcept {
    if (wave.tag == 3) {
        double value;
        if (wave.bits == 32) {
            const auto bits = u32(p, wave.big);
            float f;
            std::memcpy(&f, &bits, sizeof(f));
            value = f;
        } else {
            const auto bits = u64(p, wave.big);
            std::memcpy(&value, &bits, sizeof(value));
        }
        return std::isnan(value) ? 0.0f : static_cast<float>(std::clamp(value, -1.0, 1.0));
    }
    if (wave.tag == 6) {
        const unsigned value = p[0] ^ 0x55;
        const unsigned segment = (value >> 4) & 7;
        int magnitude = (value & 15) * 16 + (segment ? 264 : 8);
        if (segment > 1) magnitude <<= segment - 1;
        return ((value & 128) ? magnitude : -magnitude) / 32768.0f;
    }
    if (wave.tag == 7) {
        const unsigned value = p[0] ^ 0xff;
        int magnitude = ((value & 15) * 8 + 132) << ((value >> 4) & 7);
        magnitude -= 132;
        return ((value & 128) ? -magnitude : magnitude) / 32768.0f;
    }
    std::int64_t value;
    if (wave.bits == 8) value = int(p[0]) - 128;
    else {
        std::uint32_t raw = 0;
        for (unsigned i = 0; i < unsigned(wave.bits) / 8; ++i) raw = (raw << 8) | p[wave.big ? i : unsigned(wave.bits) / 8 - 1 - i];
        const auto sign = std::uint64_t(1) << (wave.bits - 1);
        value = (raw & sign) ? std::int64_t(raw) - std::int64_t(sign * 2) : raw;
    }
    if (wave.valid_bits < wave.bits) {
        const auto scale = std::int64_t(1) << (wave.bits - wave.valid_bits);
        value = value >= 0 ? value / scale * scale : -((-value + scale - 1) / scale) * scale;
    }
    return static_cast<float>(std::ldexp(static_cast<double>(value), 1 - int(wave.bits)));
}

static std::int16_t ima_sample(unsigned nibble, int& predictor, int& index) noexcept {
    constexpr int steps[] = {
        7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,107,118,
        130,143,157,173,190,209,230,253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,1060,
        1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,
        6484,7132,7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,27086,29794,32767
    };
    constexpr int changes[] = {-1,-1,-1,-1,2,4,6,8};
    const int step = steps[index];
    int delta = step >> 3;
    if (nibble & 1) delta += step >> 2;
    if (nibble & 2) delta += step >> 1;
    if (nibble & 4) delta += step;
    predictor = clamp16(predictor + ((nibble & 8) ? -delta : delta));
    index = std::clamp(index + changes[nibble & 7], 0, 88);
    return static_cast<std::int16_t>(predictor);
}

Error wave_block(const std::uint8_t* p, const Wave& wave, unsigned channels, std::vector<std::int16_t>& pcm) {
    pcm.resize(std::size_t(wave.block_frames) * channels);
    if (wave.tag == 17) {
        int predictors[2]{}, indices[2]{};
        for (unsigned c = 0; c < channels; ++c) {
            predictors[c] = static_cast<std::int16_t>(u16(p + c * 4));
            indices[c] = p[c * 4 + 2];
            if (indices[c] > 88 || p[c * 4 + 3]) return Error::invalid_data;
            pcm[c] = static_cast<std::int16_t>(predictors[c]);
        }
        unsigned frame = 1;
        for (unsigned offset = 4 * channels; offset < wave.align; offset += 4 * channels, frame += 8) {
            for (unsigned c = 0; c < channels; ++c) for (unsigned i = 0; i < 4; ++i) {
                const auto value = p[offset + c * 4 + i];
                pcm[(frame + i * 2) * channels + c] = ima_sample(value & 15, predictors[c], indices[c]);
                pcm[(frame + i * 2 + 1) * channels + c] = ima_sample(value >> 4, predictors[c], indices[c]);
            }
        }
    } else {
        constexpr int changes[] = {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};
        std::int64_t delta[2]{};
        int recent[2]{}, previous[2]{};
        std::array<std::int16_t, 2> coefficients[2];
        for (unsigned c = 0; c < channels; ++c) {
            if (p[c] >= wave.coefficients.size()) return Error::invalid_data;
            coefficients[c] = wave.coefficients[p[c]];
            delta[c] = std::max<unsigned>(16, u16(p + channels + c * 2));
            recent[c] = static_cast<std::int16_t>(u16(p + channels * 3 + c * 2));
            previous[c] = static_cast<std::int16_t>(u16(p + channels * 5 + c * 2));
            pcm[c] = static_cast<std::int16_t>(previous[c]);
            pcm[channels + c] = static_cast<std::int16_t>(recent[c]);
        }
        std::size_t out = channels * 2;
        for (unsigned offset = channels * 7; offset < wave.align; ++offset) for (unsigned half = 0; half < 2; ++half) {
            const unsigned c = channels == 2 ? half : 0;
            const unsigned nibble = half ? p[offset] & 15 : p[offset] >> 4;
            const int signed_nibble = nibble < 8 ? int(nibble) : int(nibble) - 16;
            const std::int64_t prediction = (std::int64_t(recent[c]) * coefficients[c][0] + std::int64_t(previous[c]) * coefficients[c][1]) / 256;
            const auto sample = clamp16(prediction + signed_nibble * delta[c]);
            previous[c] = recent[c];
            recent[c] = sample;
            delta[c] = std::clamp<std::int64_t>(delta[c] * changes[nibble] / 256, 16, INT32_MAX);
            pcm[out++] = sample;
        }
    }
    return Error::none;
}

}
