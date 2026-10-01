#include <aardvark_audio/audio.h>
#include <array>
#include <fstream>
#include <iostream>
#include <vector>

static void write16(std::ostream& out, std::uint16_t value) {
    const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
    out.write(bytes, 2);
}

static void write32(std::ostream& out, std::uint32_t value) {
    write16(out, static_cast<std::uint16_t>(value));
    write16(out, static_cast<std::uint16_t>(value >> 16));
}

static int decode(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::error_code ec;
    if (std::filesystem::equivalent(input, output, ec)) return 2;
    aardvark::audio::Reader reader;
    if (!reader.open(input)) {
        std::cerr << aardvark::audio::error_message(reader.error()) << '\n';
        return 1;
    }
    const auto format = reader.format();
    const auto bytes = format.frames * format.channels * 2;
    if (bytes > UINT32_MAX - 36 || format.channels > 32) return 2;
    std::ofstream out(output, std::ios::binary);
    if (!out) return 2;
    out.write("RIFF", 4);
    write32(out, static_cast<std::uint32_t>(bytes + 36));
    out.write("WAVEfmt ", 8);
    write32(out, 16);
    write16(out, 1);
    write16(out, static_cast<std::uint16_t>(format.channels));
    write32(out, format.sample_rate);
    write32(out, format.sample_rate * format.channels * 2);
    write16(out, static_cast<std::uint16_t>(format.channels * 2));
    write16(out, 16);
    out.write("data", 4);
    write32(out, static_cast<std::uint32_t>(bytes));
    std::vector<std::int16_t> buffer(4096 * format.channels);
    while (const auto frames = reader.read_s16(buffer.data(), 4096)) {
        for (std::size_t i = 0; i < frames * format.channels; ++i) write16(out, static_cast<std::uint16_t>(buffer[i]));
        if (!out) return 2;
    }
    if (reader.error() != aardvark::audio::Error::none) {
        std::cerr << aardvark::audio::error_message(reader.error()) << '\n';
        return 1;
    }
    out.close();
    if (!out) return 2;
    std::cout << format.sample_rate << " Hz, " << format.channels << " channels, " << format.frames << " frames\n";
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc != 3) { std::cerr << "Usage: aardvark_decode input output.wav\n"; return 2; }
    try { return decode(argv[1], argv[2]); }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
