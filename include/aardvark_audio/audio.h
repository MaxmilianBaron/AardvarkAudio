#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace aardvark::audio {

enum class Error {
    none,
    invalid_argument,
    io,
    invalid_data,
    unsupported,
    limit_exceeded,
    out_of_memory
};

enum class Codec { unknown, wave, mp3 };

struct Limits {
    std::uint64_t encoded_bytes = 64ull * 1024 * 1024;
    std::uint64_t decoded_bytes = 256ull * 1024 * 1024;
    std::uint32_t channels = 32;
    std::uint32_t sample_rate = 384000;
};

struct Format {
    Codec codec = Codec::unknown;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t frames = 0;
    std::uint32_t channel_mask = 0;
};

const char* error_message(Error error) noexcept;

class Reader {
public:
    Reader() noexcept;
    ~Reader();
    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool open(const std::filesystem::path& path, Limits limits = {}) noexcept;
    bool open(const void* data, std::size_t size, Limits limits = {}) noexcept;
    void close() noexcept;
    bool is_open() const noexcept;
    Format format() const noexcept;
    Error error() const noexcept;
    std::uint64_t position() const noexcept;
    bool seek(std::uint64_t frame) noexcept;
    std::size_t read_s16(std::int16_t* samples, std::size_t frames) noexcept;
    std::size_t read_f32(float* samples, std::size_t frames) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
    Error error_ = Error::none;
};

}
