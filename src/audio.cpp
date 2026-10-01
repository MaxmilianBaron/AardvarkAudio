#include "internal.h"
#include <fstream>
#include <new>

namespace aardvark::audio {

struct Reader::State {
    Format format;
    detail::Wave wave;
    std::vector<std::uint8_t> encoded;
    std::vector<float> decoded;
    std::vector<std::int16_t> block;
    std::uint64_t position = 0;
    std::uint64_t cached_block = UINT64_MAX;

    Error initialize(Limits limits) {
        if (!limits.channels || !limits.sample_rate || !limits.encoded_bytes || !limits.decoded_bytes) return Error::invalid_argument;
        if (encoded.size() < 4) return Error::invalid_data;
        if (!std::memcmp(encoded.data(), "RIFF", 4) || !std::memcmp(encoded.data(), "RIFX", 4) || !std::memcmp(encoded.data(), "RF64", 4)) {
            return detail::parse_wave(encoded, limits, format, wave);
        }
        return detail::decode_mp3(encoded, limits, format, decoded);
    }

    template<class Sample>
    std::size_t read(Sample* output, std::size_t requested, Error& error) {
        if (!requested) return 0;
        if (!output || requested > SIZE_MAX / format.channels) {
            error = Error::invalid_argument;
            return 0;
        }
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(requested, format.frames - position));
        for (std::size_t frame = 0; frame < count; ++frame) {
            if (wave.block_frames) {
                const auto index = position / wave.block_frames;
                if (cached_block != index) {
                    error = detail::wave_block(encoded.data() + wave.data + static_cast<std::size_t>(index) * wave.align, wave, format.channels, block);
                    if (error != Error::none) return frame;
                    cached_block = index;
                }
            }
            for (std::uint32_t channel = 0; channel < format.channels; ++channel) {
                float value;
                if (format.codec == Codec::mp3) value = decoded[static_cast<std::size_t>(position) * format.channels + channel];
                else if (wave.block_frames) value = block[static_cast<std::size_t>(position % wave.block_frames) * format.channels + channel] / 32768.0f;
                else value = detail::wave_sample(encoded.data() + wave.data + static_cast<std::size_t>(position) * wave.align + channel * (wave.bits / 8), wave);
                if constexpr (std::is_same_v<Sample, float>) *output++ = value;
                else *output++ = detail::to_s16(value);
            }
            ++position;
        }
        error = Error::none;
        return count;
    }
};

Reader::Reader() noexcept = default;
Reader::~Reader() = default;
Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;

bool Reader::open(const std::filesystem::path& path, Limits limits) noexcept {
    close();
    try {
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) { error_ = Error::io; return false; }
        const auto end = input.tellg();
        if (end < 0) { error_ = Error::io; return false; }
        const auto size = static_cast<std::uint64_t>(end);
        if (size > limits.encoded_bytes || size > SIZE_MAX || size > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
            error_ = Error::limit_exceeded;
            return false;
        }
        auto next = std::make_unique<State>();
        next->encoded.resize(static_cast<std::size_t>(size));
        input.seekg(0);
        if (size && !input.read(reinterpret_cast<char*>(next->encoded.data()), static_cast<std::streamsize>(size))) {
            error_ = Error::io;
            return false;
        }
        error_ = next->initialize(limits);
        if (error_ != Error::none) return false;
        state_ = std::move(next);
        return true;
    } catch (const std::bad_alloc&) { error_ = Error::out_of_memory; }
    catch (...) { error_ = Error::io; }
    return false;
}

bool Reader::open(const void* data, std::size_t size, Limits limits) noexcept {
    close();
    if (!data && size) { error_ = Error::invalid_argument; return false; }
    if (size > limits.encoded_bytes) { error_ = Error::limit_exceeded; return false; }
    try {
        auto next = std::make_unique<State>();
        if (size) {
            const auto* first = static_cast<const std::uint8_t*>(data);
            next->encoded.assign(first, first + size);
        }
        error_ = next->initialize(limits);
        if (error_ != Error::none) return false;
        state_ = std::move(next);
        return true;
    } catch (const std::bad_alloc&) { error_ = Error::out_of_memory; }
    catch (...) { error_ = Error::invalid_data; }
    return false;
}

void Reader::close() noexcept { state_.reset(); error_ = Error::none; }
bool Reader::is_open() const noexcept { return state_ != nullptr; }
Format Reader::format() const noexcept { return state_ ? state_->format : Format{}; }
Error Reader::error() const noexcept { return error_; }
std::uint64_t Reader::position() const noexcept { return state_ ? state_->position : 0; }

bool Reader::seek(std::uint64_t frame) noexcept {
    if (!state_ || frame > state_->format.frames) { error_ = Error::invalid_argument; return false; }
    state_->position = frame;
    error_ = Error::none;
    return true;
}

std::size_t Reader::read_s16(std::int16_t* samples, std::size_t frames) noexcept {
    if (!state_) { error_ = Error::invalid_argument; return 0; }
    try { return state_->read(samples, frames, error_); }
    catch (const std::bad_alloc&) { error_ = Error::out_of_memory; }
    catch (...) { error_ = Error::invalid_data; }
    return 0;
}

std::size_t Reader::read_f32(float* samples, std::size_t frames) noexcept {
    if (!state_) { error_ = Error::invalid_argument; return 0; }
    try { return state_->read(samples, frames, error_); }
    catch (const std::bad_alloc&) { error_ = Error::out_of_memory; }
    catch (...) { error_ = Error::invalid_data; }
    return 0;
}

const char* error_message(Error error) noexcept {
    switch (error) {
    case Error::none: return "No error";
    case Error::invalid_argument: return "Invalid argument";
    case Error::io: return "File could not be read";
    case Error::invalid_data: return "Invalid or truncated audio data";
    case Error::unsupported: return "Unsupported audio format";
    case Error::limit_exceeded: return "Audio exceeds configured limits";
    case Error::out_of_memory: return "Insufficient memory";
    }
    return "Unknown error";
}

}
