#include <aardvark_audio/audio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <vector>

using namespace aardvark::audio;
using Bytes = std::vector<std::uint8_t>;
static unsigned checks = 0;

static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::cerr << name << '\n'; std::exit(1); }
}

static Bytes silent(unsigned version = 3, bool mono = false, bool crc = false, bool free = false) {
    const unsigned rate = version == 3 ? 44100 : version == 2 ? 22050 : 11025;
    const unsigned length = (version == 3 ? 144000 : 72000) * 128 / rate;
    Bytes frame(length);
    frame[0] = 255; frame[1] = static_cast<std::uint8_t>(0xe2 | (version << 3) | !crc);
    frame[2] = static_cast<std::uint8_t>(free ? 0 : version == 3 ? 0x90 : 0xc0);
    frame[3] = mono ? 0xc0 : 0;
    if (crc) {
        unsigned checksum = 65535;
        const unsigned side = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        for (unsigned i = 0; i < side + 2; ++i) {
            const unsigned value = i < 2 ? frame[i + 2] : frame[i + 4];
            for (unsigned bit = 128; bit; bit >>= 1) {
                const bool feedback = ((checksum >> 15) != 0) != ((value & bit) != 0);
                checksum = ((checksum << 1) & 65535) ^ (feedback ? 0x8005 : 0);
            }
        }
        frame[4] = static_cast<std::uint8_t>(checksum >> 8); frame[5] = static_cast<std::uint8_t>(checksum);
    }
    return frame;
}

static bool decode_silence(const Bytes& data, unsigned rate, unsigned channels, unsigned frames) {
    Reader reader;
    if (!reader.open(data.data(), data.size())) return false;
    const auto f = reader.format();
    if (f.codec != Codec::mp3 || f.sample_rate != rate || f.channels != channels || f.frames != frames) return false;
    std::array<float, 514> samples{};
    while (reader.position() < f.frames) {
        samples.fill(1);
        const auto count = reader.read_f32(samples.data(), samples.size() / channels);
        if (!count || !std::all_of(samples.begin(), samples.begin() + count * channels, [](float v) { return v == 0; })) return false;
    }
    return reader.seek(13) && reader.read_f32(samples.data(), 1) == 1 && reader.position() == 14;
}

static void frames() {
    for (unsigned version : {0u, 2u, 3u}) for (bool mono : {false, true}) for (bool crc : {false, true}) {
        const auto frame = silent(version, mono, crc);
        Bytes file;
        for (unsigned i = 0; i < 3; ++i) file.insert(file.end(), frame.begin(), frame.end());
        const unsigned rate = version == 3 ? 44100 : version == 2 ? 22050 : 11025;
        const unsigned count = 3 * (version == 3 ? 1152 : 576);
        check(decode_silence(file, rate, mono ? 1 : 2, count), "Version, channel, CRC and exact seek");
        if (crc) {
            file[4] ^= 1;
            Reader reader;
            check(!reader.open(file.data(), file.size()) && reader.error() == Error::invalid_data, "CRC rejects damaged side information");
        }
    }
    auto frame = silent(3, false, false, true);
    Bytes free;
    for (unsigned i = 0; i < 3; ++i) free.insert(free.end(), frame.begin(), frame.end());
    check(decode_silence(free, 44100, 2, 3456), "Free format frames");
    auto ordinary = silent();
    std::array<std::future<bool>, 8> jobs;
    for (auto& job : jobs) job = std::async(std::launch::async, [&] { return decode_silence(ordinary, 44100, 2, 1152); });
    for (auto& job : jobs) check(job.get(), "Independent concurrent readers");
}

static void tags() {
    const auto frame = silent();
    Bytes tagged = {'I','D','3',4,0,0,0,0,0,3,1,2,3};
    tagged.insert(tagged.end(), frame.begin(), frame.end());
    Bytes tail(128); tail[0] = 'T'; tail[1] = 'A'; tail[2] = 'G';
    tagged.insert(tagged.end(), tail.begin(), tail.end());
    check(decode_silence(tagged, 44100, 2, 1152), "ID3v2 and ID3v1");
    tagged[6] = 128;
    Reader reader;
    check(!reader.open(tagged.data(), tagged.size()), "Malformed ID3 length");
    auto info = frame;
    std::copy_n("Xing", 4, info.begin() + 36);
    info.insert(info.end(), frame.begin(), frame.end());
    check(decode_silence(info, 44100, 2, 1152), "Xing frame is metadata");
    info[43] = 16;
    check(!reader.open(info.data(), info.size()), "Reserved Xing flags");
}

static void invalid_inputs() {
    auto file = silent();
    Reader reader;
    for (std::size_t n = 0; n < file.size(); ++n) check(!reader.open(file.data(), n), "Partial frame rejected");
    auto damaged = file;
    damaged[4] = 128;
    check(!reader.open(damaged.data(), damaged.size()), "Unavailable reservoir rejected");
    damaged = file; damaged[1] = 0xff;
    check(!reader.open(damaged.data(), damaged.size()), "Layer I unsupported");
    damaged = file; damaged[1] = 0xf3;
    check(!reader.open(damaged.data(), damaged.size()), "Mismatched version side info");
    Limits limits; limits.decoded_bytes = 100;
    check(!reader.open(file.data(), file.size(), limits) && reader.error() == Error::limit_exceeded, "Decoded limit");
    limits = {}; limits.channels = 1;
    check(!reader.open(file.data(), file.size(), limits) && reader.error() == Error::limit_exceeded, "Channel limit");
    limits = {}; limits.sample_rate = 8000;
    check(!reader.open(file.data(), file.size(), limits) && reader.error() == Error::limit_exceeded, "Rate limit");
    limits = {}; limits.encoded_bytes = file.size() - 1;
    check(!reader.open(file.data(), file.size(), limits) && reader.error() == Error::limit_exceeded, "Encoded limit");
    limits = {}; limits.decoded_bytes = 32768;
    for (std::uint32_t seed = 1; seed <= 4096; ++seed) {
        damaged = file;
        auto rng = seed;
        for (unsigned i = 0; i < 12; ++i) {
            rng = rng * 1664525 + 1013904223;
            const auto at = rng % damaged.size();
            rng = rng * 1664525 + 1013904223;
            damaged[at] = static_cast<std::uint8_t>(rng >> 24);
        }
        if (reader.open(damaged.data(), damaged.size(), limits)) {
            std::array<float, 2304> samples{};
            const auto n = reader.read_f32(samples.data(), samples.size() / reader.format().channels);
            check(n <= 2304 && std::all_of(samples.begin(), samples.end(), [](float f) { return std::isfinite(f) && f >= -1 && f <= 1; }), "Mutated frame yields bounded PCM");
        } else check(!reader.is_open(), "Mutated frame closes cleanly");
    }
}

int main() {
    frames(); tags(); invalid_inputs();
    std::cout << checks << " checks passed\n";
}
