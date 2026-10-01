#include <aardvark_audio/audio.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

using namespace aardvark::audio;
using Bytes = std::vector<std::uint8_t>;
static unsigned checks = 0;

static void check(bool condition, const char* name) {
    ++checks;
    if (!condition) { std::cerr << name << '\n'; std::exit(1); }
}

static void number(Bytes& b, std::uint64_t value, unsigned width, bool big = false) {
    for (unsigned i = 0; i < width; ++i) b.push_back(static_cast<std::uint8_t>(value >> (8 * (big ? width - i - 1 : i))));
}

static void label(Bytes& b, const char* s) { b.insert(b.end(), s, s + 4); }
static void chunk(Bytes& b, const char* s, Bytes data, bool big = false) {
    label(b, s); number(b, data.size(), 4, big);
    b.insert(b.end(), data.begin(), data.end());
    if (data.size() & 1) b.push_back(0);
}

static Bytes make_wave(unsigned tag, unsigned bits, unsigned channels, const Bytes& pcm, bool big = false, Bytes extra = {}) {
    Bytes body, format;
    label(body, "WAVE");
    number(format, tag, 2, big); number(format, channels, 2, big);
    number(format, 48000, 4, big); number(format, 48000 * channels * (bits / 8), 4, big);
    number(format, channels * (bits / 8), 2, big); number(format, bits, 2, big);
    format.insert(format.end(), extra.begin(), extra.end());
    chunk(body, "JUNK", {1,2,3}, big);
    chunk(body, "fmt ", format, big); chunk(body, "data", pcm, big);
    Bytes result;
    label(result, big ? "RIFX" : "RIFF"); number(result, body.size(), 4, big);
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

static void integers() {
    for (unsigned bits : {8u,16u,24u,32u}) for (bool big : {false,true}) {
        Bytes pcm;
        const auto midpoint = std::uint64_t(1) << (bits - 1);
        for (const auto value : {midpoint, midpoint / 2, std::uint64_t(0), midpoint * 3 / 2, midpoint - 1}) number(pcm, value, bits / 8, big);
        const auto file = make_wave(1, bits, 1, pcm, big);
        Reader reader;
        check(reader.open(file.data(), file.size()), "Integer wave open");
        check(reader.format().frames == 5 && reader.format().sample_rate == 48000, "Integer format");
        std::array<float,5> actual{};
        check(reader.read_f32(actual.data(), actual.size()) == actual.size(), "Integer read");
        const std::array<float,5> expected = bits == 8 ? std::array<float,5>{0,-0.5f,-1,0.5f,-1/128.0f} :
            std::array<float,5>{-1,0.5f,0,-0.5f,static_cast<float>(1.0 - 1.0 / midpoint)};
        for (unsigned i = 0; i < 5; ++i) check(actual[i] == expected[i], "Integer conversion");
        check(reader.read_f32(actual.data(), 1) == 0 && reader.error() == Error::none, "Integer EOF");
        check(reader.seek(0), "Integer rewind");
        std::array<std::int16_t,5> shorts{};
        check(reader.read_s16(shorts.data(), shorts.size()) == shorts.size(), "Integer s16");
        if (bits != 8) check(shorts[0] == -32768 && shorts[1] == 16384 && shorts[2] == 0 && shorts[3] == -16384 && shorts[4] == 32767, "Integer s16 values");
        check(!reader.seek(6) && reader.position() == 5, "Seek out of bounds");
        check(reader.seek(2) && reader.read_s16(shorts.data(), 1) == 1 && shorts[0] == (bits == 8 ? -32768 : 0), "Seek exact frame");
        Reader moved(std::move(reader));
        check(moved.is_open() && !reader.is_open() && moved.position() == 3, "Reader move");
    }
}

static void floating() {
    for (unsigned bits : {32u,64u}) for (bool big : {false,true}) {
        Bytes pcm;
        for (double value : {-2.0,-1.0,-0.5,0.0,0.5,1.0,2.0,std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
            std::uint64_t raw = 0;
            if (bits == 32) { float f = static_cast<float>(value); std::uint32_t word; std::memcpy(&word, &f, 4); raw = word; }
            else std::memcpy(&raw, &value, 8);
            number(pcm, raw, bits / 8, big);
        }
        const auto file = make_wave(3, bits, 1, pcm, big);
        Reader reader;
        check(reader.open(file.data(), file.size()), "Float open");
        std::array<std::int16_t,10> output{};
        check(reader.read_s16(output.data(), 10) == 10, "Float read");
        check(output == std::array<std::int16_t,10>{-32768,-32768,-16384,0,16384,32767,32767,32767,-32768,0}, "Float clipping and nonfinite");
    }
}

static void companding() {
    Reader reader;
    auto wave = make_wave(6, 8, 1, {0xd5,0x55,0xaa,0x2a});
    check(reader.open(wave.data(), wave.size()), "A-law open");
    std::array<std::int16_t,4> output{};
    check(reader.read_s16(output.data(), 4) == 4, "A-law read");
    check(output == std::array<std::int16_t,4>{8,-8,32256,-32256}, "A-law values");
    wave = make_wave(7, 8, 1, {0xff,0x7f,0x80,0x00});
    check(reader.open(wave.data(), wave.size()), "Mu-law open");
    check(reader.read_s16(output.data(), 4) == 4, "Mu-law read");
    check(output == std::array<std::int16_t,4>{0,0,32124,-32124}, "Mu-law values");
}

static void extended() {
    Bytes extra;
    number(extra,22,2); number(extra,20,2); number(extra,3,4);
    const std::uint8_t guid[] = {1,0,0,0,0,0,0x10,0,0x80,0,0,0xaa,0,0x38,0x9b,0x71};
    extra.insert(extra.end(), std::begin(guid), std::end(guid));
    auto wave = make_wave(0xfffe,24,2,{0xff,0xff,0x7f,0x01,0x00,0x80},false,extra);
    Reader reader;
    check(reader.open(wave.data(),wave.size()), "Extensible open");
    check(reader.format().channel_mask == 3 && reader.format().frames == 1, "Extensible format");
    std::array<float,2> samples{};
    check(reader.read_f32(samples.data(),1) == 1 && samples[0] == 1.0f-1.0f/524288 && samples[1] == -1, "Valid bits alignment");
    wave[24+8+24] = 2;
    check(!reader.open(wave.data(),wave.size()), "Unsupported extensible subformat");
    wave = make_wave(1,16,1,{0,0,0xff,0x7f});
    Bytes rf64;
    label(rf64,"RF64"); number(rf64,UINT32_MAX,4); label(rf64,"WAVE");
    Bytes ds;
    number(ds,wave.size()+36-8,8); number(ds,4,8); number(ds,2,8); number(ds,0,4);
    chunk(rf64,"ds64",ds);
    rf64.insert(rf64.end(),wave.begin()+12,wave.end());
    for (std::size_t i = rf64.size()-8; i < rf64.size()-4; ++i) rf64[i] = 0xff;
    check(reader.open(rf64.data(),rf64.size()) && reader.format().frames == 2, "RF64 open");
    rf64[20] = 0xff;
    check(!reader.open(rf64.data(),rf64.size()), "RF64 truncated");
}

static Bytes adpcm(unsigned tag, unsigned channels, unsigned align, unsigned frames, Bytes data) {
    Bytes body, format, fact;
    label(body,"WAVE");
    number(format,tag,2); number(format,channels,2); number(format,8000,4); number(format,8000*align/frames,4);
    number(format,align,2); number(format,4,2);
    number(format,tag==17?2:8,2); number(format,frames,2);
    if (tag==2) { number(format,1,2); number(format,256,2); number(format,0,2); }
    chunk(body,"fmt ",format); number(fact,frames,4); chunk(body,"fact",fact); chunk(body,"data",data);
    Bytes result; label(result,"RIFF"); number(result,body.size(),4); result.insert(result.end(),body.begin(),body.end());
    return result;
}

static void compressed() {
    for (unsigned channels : {1u,2u}) {
        auto file = adpcm(17,channels,channels*8,9,Bytes(channels*8));
        Reader reader;
        check(reader.open(file.data(),file.size()), "IMA open");
        std::array<std::int16_t,18> output;
        output.fill(-1);
        check(reader.read_s16(output.data(),9) == 9, "IMA read");
        check(std::all_of(output.begin(),output.begin()+9*channels,[](int v){return v==0;}), "IMA zero block");
        check(reader.seek(3) && reader.read_s16(output.data(),1)==1 && output[0]==0, "IMA seek");
        file[file.size()-channels*8+2]=89;
        check(reader.open(file.data(),file.size()), "IMA malformed open is bounded");
        check(reader.read_s16(output.data(),9)==0 && reader.error()==Error::invalid_data, "IMA index range");
    }
    auto file=adpcm(2,1,8,4,{0,16,0,32,0,16,0,0x1f});
    Reader reader;
    check(reader.open(file.data(),file.size()), "MS ADPCM open");
    std::array<std::int16_t,4> output{};
    check(reader.read_s16(output.data(),4)==4, "MS ADPCM read");
    check(output==std::array<std::int16_t,4>{16,32,48,32}, "MS ADPCM predictor");
}

static void invalid_inputs() {
    Reader reader;
    check(!reader.open(nullptr,1) && reader.error()==Error::invalid_argument, "Null buffer");
    check(!reader.open(nullptr,0), "Empty buffer");
    auto file=make_wave(1,16,2,{0,0,0,0,1,0,2,0});
    for (std::size_t i=0;i<file.size();++i) check(!reader.open(file.data(),i), "All truncations rejected");
    Limits limits;
    limits.encoded_bytes=file.size()-1;
    check(!reader.open(file.data(),file.size(),limits) && reader.error()==Error::limit_exceeded, "Encoded limit");
    limits={}; limits.decoded_bytes=15;
    check(!reader.open(file.data(),file.size(),limits), "Decoded limit");
    limits={}; limits.channels=1;
    check(!reader.open(file.data(),file.size(),limits), "Channel limit");
    limits={}; limits.sample_rate=44100;
    check(!reader.open(file.data(),file.size(),limits), "Sample rate limit");
    check(reader.open(file.data(),file.size()), "Reopen after errors");
    check(reader.read_s16(nullptr,1)==0 && reader.error()==Error::invalid_argument && reader.position()==0, "Null output");
    std::int16_t sample=0;
    check(reader.read_s16(&sample,SIZE_MAX)==0, "Output overflow rejected");
    check(reader.read_s16(nullptr,0)==0 && reader.position()==0, "Zero read");
    for (std::uint32_t value=1;value<30000;++value) {
        auto changed=file;
        auto rng=value;
        for (unsigned i=0;i<5;++i) { rng=rng*1664525+1013904223; const auto index=rng%changed.size(); rng=rng*1664525+1013904223; changed[index]=static_cast<std::uint8_t>(rng>>24); }
        if (reader.open(changed.data(),changed.size())) {
            std::array<float,256> output{};
            const auto count=reader.read_f32(output.data(),output.size()/reader.format().channels);
            check(count<=128,"Mutation read bounded");
        } else check(!reader.is_open(),"Mutation rejects cleanly");
    }
}

int main() {
    integers(); floating(); companding(); extended(); compressed(); invalid_inputs();
    std::cout<<checks<<" checks passed\n";
}
