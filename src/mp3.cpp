#include "internal.h"
#include "mp3_codewords.h"
#include "mp3_tables.h"

namespace aardvark::audio::detail {
namespace {

struct Invalid {};

struct Bits {
    const std::uint8_t* bytes;
    std::size_t pos, end;
    unsigned take(unsigned count) {
        if (count > 24 || pos > end || count > end - pos) throw Invalid{};
        unsigned value = 0;
        for (unsigned i = 0; i < count; ++i, ++pos) value = (value << 1) | ((bytes[pos / 8] >> (7 - pos % 8)) & 1);
        return value;
    }
};

struct Header {
    unsigned version = 0, rate = 0, rate_index = 0, channels = 0, bytes = 0, mode = 0, extension = 0;
    unsigned side = 0, padding = 0;
    bool crc = false;
};

bool header(const std::uint8_t* p, Header& h) {
    const auto bits = u32(p, true);
    if ((bits & 0xffe00000u) != 0xffe00000u || ((bits >> 17) & 3) != 1) return false;
    h.version = (bits >> 19) & 3;
    h.rate_index = (bits >> 10) & 3;
    const unsigned bitrate = (bits >> 12) & 15;
    if (h.version == 1 || h.rate_index == 3 || bitrate == 15 || (bits & 3) == 2) return false;
    constexpr unsigned rates[] = {44100,48000,32000};
    constexpr unsigned rates1[] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    constexpr unsigned rates2[] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    h.rate = rates[h.rate_index] >> (h.version == 3 ? 0 : h.version == 2 ? 1 : 2);
    h.padding = (bits >> 9) & 1;
    h.mode = (bits >> 6) & 3;
    h.extension = h.mode == 1 ? (bits >> 4) & 3 : 0;
    h.channels = h.mode == 3 ? 1 : 2;
    h.crc = !(bits & 0x10000);
    h.side = h.version == 3 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
    h.bytes = bitrate ? (h.version == 3 ? 144000 * rates1[bitrate] : 72000 * rates2[bitrate]) / h.rate + h.padding : 0;
    return !h.bytes || h.bytes >= 4 + (h.crc ? 2 : 0) + h.side;
}

bool compatible(const Header& a, const Header& b) { return a.version == b.version && a.rate == b.rate && a.channels == b.channels; }

struct Granule {
    unsigned length = 0, pairs = 0, gain = 0, compress = 0, block = 0, region0 = 0, region1 = 0;
    unsigned book[3]{}, subgain[3]{};
    bool mixed = false, pre = false, scale = false, quad = false;
    unsigned sf[23]{}, ssf[13][3]{}, illegal[23]{}, sillegal[13][3]{};
    std::array<double, 576> spectral{};
};

void side_info(Bits& input, const Header& h, Granule (&g)[2][2], unsigned (&reuse)[2], unsigned& back) {
    back = input.take(h.version == 3 ? 9 : 8);
    input.take(h.version == 3 ? (h.channels == 1 ? 5 : 3) : (h.channels == 1 ? 1 : 2));
    if (h.version == 3) for (unsigned ch = 0; ch < h.channels; ++ch) reuse[ch] = input.take(4);
    for (unsigned granule = 0; granule < (h.version == 3 ? 2u : 1u); ++granule) for (unsigned ch = 0; ch < h.channels; ++ch) {
        auto& v = g[granule][ch];
        v.length = input.take(12);
        v.pairs = input.take(9);
        v.gain = input.take(8);
        v.compress = input.take(h.version == 3 ? 4 : 9);
        if (v.pairs > 288) throw Invalid{};
        if (input.take(1)) {
            v.block = input.take(2);
            if (!v.block) throw Invalid{};
            v.mixed = input.take(1) != 0;
            v.book[0] = input.take(5); v.book[1] = input.take(5);
            for (auto& gain : v.subgain) gain = input.take(3);
            v.region0 = v.block == 2 && !v.mixed ? 8 : 7;
            v.region1 = 20 - v.region0;
        } else {
            for (auto& book : v.book) book = input.take(5);
            v.region0 = input.take(4); v.region1 = input.take(3);
            if (v.region0 + v.region1 > 20) throw Invalid{};
        }
        for (auto book : v.book) if (book == 4 || book == 14) throw Invalid{};
        v.pre = h.version == 3 && input.take(1);
        v.scale = input.take(1) != 0;
        v.quad = input.take(1) != 0;
    }
    if (input.pos != input.end) throw Invalid{};
}

void scalefactors(Bits& input, Granule& g, const Granule& previous, const Header& h, unsigned channel, unsigned reuse) {
    if (h.version == 3) {
        constexpr unsigned widths[16][2] = {{0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},{2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}};
        const auto* w = widths[g.compress];
        if (g.block == 2) {
            if (g.mixed) for (unsigned s = 0; s < 8; ++s) g.sf[s] = input.take(w[0]);
            for (unsigned s = g.mixed ? 3 : 0; s < 12; ++s) for (unsigned k = 0; k < 3; ++k) g.ssf[s][k] = input.take(w[s < 6 ? 0 : 1]);
        } else {
            constexpr unsigned groups[] = {0,6,11,16,21};
            for (unsigned part = 0; part < 4; ++part) for (unsigned s = groups[part]; s < groups[part + 1]; ++s)
                g.sf[s] = reuse & (8 >> part) ? previous.sf[s] : input.take(w[part < 2 ? 0 : 1]);
        }
        for (auto& v : g.illegal) v = 7;
        for (auto& band : g.sillegal) for (auto& v : band) v = 7;
        return;
    }
    constexpr unsigned counts[6][3][4] = {
        {{6,5,5,5},{9,9,9,9},{6,9,9,9}},
        {{6,5,7,3},{9,9,12,6},{6,9,12,6}},
        {{11,10,0,0},{18,18,0,0},{15,18,0,0}},
        {{7,7,7,0},{12,12,12,0},{6,15,12,0}},
        {{6,6,6,3},{12,9,9,6},{6,12,9,6}},
        {{8,8,5,0},{15,12,9,0},{6,18,9,0}}
    };
    unsigned slen[4]{}, group = 0, c = g.compress;
    const bool intensity = channel == 1 && (h.extension & 1);
    if (intensity) {
        c >>= 1;
        if (c < 180) { group = 3; slen[0] = c / 36; slen[1] = c / 6 % 6; slen[2] = c % 6; }
        else if (c < 244) { group = 4; c -= 180; slen[0] = c / 16; slen[1] = c / 4 % 4; slen[2] = c % 4; }
        else { group = 5; c -= 244; slen[0] = c / 3; slen[1] = c % 3; }
    } else if (c < 400) { slen[0] = (c >> 4) / 5; slen[1] = (c >> 4) % 5; slen[2] = (c >> 2) % 4; slen[3] = c % 4; }
    else if (c < 500) { group = 1; c -= 400; slen[0] = (c >> 2) / 5; slen[1] = (c >> 2) % 5; slen[2] = c % 4; }
    else { group = 2; c -= 500; slen[0] = c / 3; slen[1] = c % 3; g.pre = true; }
    unsigned values[54]{}, invalid[54]{}, at = 0;
    const unsigned kind = g.block == 2 ? (g.mixed ? 2 : 1) : 0;
    for (unsigned part = 0; part < 4; ++part) for (unsigned n = 0; n < counts[group][kind][part]; ++n) {
        values[at] = input.take(slen[part]); invalid[at++] = slen[part] ? (1u << slen[part]) - 1 : UINT32_MAX;
    }
    at = 0;
    if (g.block != 2) {
        for (unsigned s = 0; s < 21; ++s) { g.sf[s] = values[at]; g.illegal[s] = invalid[at++]; }
    } else {
        if (g.mixed) for (unsigned s = 0; s < 6; ++s) { g.sf[s] = values[at]; g.illegal[s] = invalid[at++]; }
        for (unsigned s = g.mixed ? 3 : 0; s < 12; ++s) for (unsigned k = 0; k < 3; ++k) {
            g.ssf[s][k] = values[at]; g.sillegal[s][k] = invalid[at++];
        }
    }
}

struct Codebook {
    struct Node { int next[2] = {-1,-1}; int value = -1; };
    std::vector<Node> nodes{1};
    void add(std::uint32_t code, unsigned value) {
        unsigned length = 0;
        for (auto n = code; n > 1; n >>= 1) ++length;
        int node = 0;
        for (unsigned n = length; n; --n) {
            const unsigned bit = (code >> (n - 1)) & 1;
            if (nodes[node].next[bit] < 0) {
                const auto next = static_cast<int>(nodes.size());
                nodes[node].next[bit] = next;
                nodes.emplace_back();
            }
            node = nodes[node].next[bit];
        }
        nodes[node].value = static_cast<int>(value);
    }
    unsigned read(Bits& input) const {
        int node = 0;
        for (unsigned n = 0; n < 20; ++n) {
            if (nodes[node].value >= 0) return static_cast<unsigned>(nodes[node].value);
            node = nodes[node].next[input.take(1)];
            if (node < 0) throw Invalid{};
        }
        throw Invalid{};
    }
};

struct HuffmanTables {
    Codebook books[34];
    template<std::size_t N> void load(unsigned id, const std::uint32_t (&codes)[N], unsigned width) {
        for (unsigned i = 0; i < N; ++i) books[id].add(codes[i], (i / width) * 16 + i % width);
    }
    HuffmanTables() {
        books[0].add(1, 0);
        load(1,codewords_1,2); load(2,codewords_2,3); load(3,codewords_3,3);
        load(5,codewords_5,4); load(6,codewords_6,4);
        load(7,codewords_7,6); load(8,codewords_8,6); load(9,codewords_9,6);
        load(10,codewords_10,8); load(11,codewords_11,8); load(12,codewords_12,8);
        load(13,codewords_13,16); load(15,codewords_15,16); load(16,codewords_16,16); load(24,codewords_24,16);
        constexpr unsigned lengths[] = {1,4,4,5,4,6,5,6,4,5,5,6,5,6,6,6};
        constexpr unsigned codes[] = {1,5,4,5,6,5,4,4,7,3,6,0,7,2,3,1};
        for (unsigned i = 0; i < 16; ++i) { books[32].add((1u << lengths[i]) | codes[i], i); books[33].add(16 | (15 - i), i); }
    }
};

void spectrum(Bits& input, Granule& g, const Bands& b, const Header& h) {
    static const HuffmanTables tables;
    constexpr unsigned extra[] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,3,4,6,8,10,13,4,5,6,7,8,9,11,13};
    const unsigned boundary0 = g.block == 2 ? (h.rate == 8000 ? 72 : 36) : b.long_lines[g.region0 + 1];
    const unsigned boundary1 = g.block ? 576 : b.long_lines[g.region0 + g.region1 + 2];
    unsigned at = 0;
    for (unsigned pair = 0; pair < g.pairs; ++pair) {
        const unsigned book = g.book[at < boundary0 ? 0 : at < boundary1 ? 1 : 2];
        const unsigned id = book >= 24 ? 24 : book >= 16 ? 16 : book;
        const unsigned value = tables.books[id].read(input);
        for (unsigned half = 0; half < 2; ++half) {
            unsigned n = half ? value & 15 : value >> 4;
            if (n == 15) n += input.take(extra[book]);
            const bool negative = n && input.take(1);
            g.spectral[at++] = negative ? -static_cast<double>(n) : n;
        }
    }
    while (at <= 572 && input.pos < input.end) {
        const auto before = input.pos;
        try {
            const auto value = tables.books[g.quad ? 33 : 32].read(input);
            double quad[4]{};
            for (unsigned k = 0; k < 4; ++k) if (value & (8 >> k)) quad[k] = input.take(1) ? -1 : 1;
            for (auto v : quad) g.spectral[at++] = v;
        } catch (const Invalid&) { input.pos = before; break; }
    }
    input.pos = input.end;
    const auto quantized = g.spectral;
    constexpr unsigned pre[] = {0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,3,2,0};
    const double gain = (static_cast<int>(g.gain) - 210) / 4.0;
    const double scale = g.scale ? 1.0 : 0.5;
    const unsigned long_bands = g.block == 2 ? (g.mixed ? (h.version == 3 ? 8 : 6) : 0) : 22;
    at = 0;
    auto requantize = [](double value, double multiplier) {
        return std::copysign(std::pow(std::abs(value), 4.0 / 3.0) * multiplier, value);
    };
    for (unsigned s = 0; s < long_bands; ++s) {
        const auto multiplier = std::exp2(gain - scale * (g.sf[s] + (g.pre ? pre[s] : 0)));
        for (unsigned i = b.long_lines[s]; i < b.long_lines[s + 1]; ++i) g.spectral[i] = requantize(quantized[at++], multiplier);
    }
    if (g.block == 2) for (unsigned s = g.mixed ? 3 : 0; s < 13; ++s) for (unsigned w = 0; w < 3; ++w) {
        const auto multiplier = std::exp2(gain - 2.0 * g.subgain[w] - scale * g.ssf[s][w]);
        for (unsigned i = b.short_lines[s]; i < b.short_lines[s + 1]; ++i) g.spectral[3 * i + w] = requantize(quantized[at++], multiplier);
    }
    if (at != 576) throw Invalid{};
}

void stereo(Granule& left, Granule& right, const Header& h, const Bands& b) {
    std::array<bool, 576> intensity{};
    auto apply = [&](unsigned begin, unsigned end, unsigned step, unsigned pos, unsigned illegal) {
        if (pos == illegal) return;
        double kl = 1, kr = 1;
        if (h.version == 3) {
            if (pos > 6) return;
            if (pos == 6) kr = 0;
            else { const auto ratio = std::tan(pos * 3.14159265358979323846 / 12); kr = 1 / (1 + ratio); kl = ratio * kr; }
        } else {
            const double exponent = (right.compress & 1) ? -0.5 : -0.25;
            if (pos & 1) kl = std::exp2(exponent * ((pos + 1) / 2)); else kr = std::exp2(exponent * (pos / 2));
        }
        for (unsigned i = begin; i < end; i += step) {
            right.spectral[i] = left.spectral[i] * kr; left.spectral[i] *= kl; intensity[i] = true;
        }
    };
    if (h.extension & 1) {
        const unsigned long_count = right.block == 2 ? (right.mixed ? (h.version == 3 ? 8 : 6) : 0) : 22;
        unsigned last = 0;
        for (unsigned s = 0; s < long_count; ++s) for (unsigned i = b.long_lines[s]; i < b.long_lines[s + 1]; ++i) if (right.spectral[i] != 0) last = s + 1;
        bool short_nonzero = false;
        if (right.block == 2) {
            for (unsigned w = 0; w < 3; ++w) {
                unsigned start = right.mixed ? 3 : 0;
                for (unsigned s = start; s < 13; ++s) for (unsigned i = b.short_lines[s]; i < b.short_lines[s + 1]; ++i) if (right.spectral[i * 3 + w] != 0) { start = s + 1; short_nonzero = true; }
                for (unsigned s = start; s < 13; ++s) {
                    const unsigned sf = std::min(s, 11u);
                    apply(b.short_lines[s] * 3 + w, b.short_lines[s + 1] * 3, 3, right.ssf[sf][w], right.sillegal[sf][w]);
                }
            }
        }
        if (!short_nonzero) for (unsigned s = last; s < long_count; ++s) {
            const unsigned sf = std::min(s, 20u);
            apply(b.long_lines[s], b.long_lines[s + 1], 1, right.sf[sf], right.illegal[sf]);
        }
    }
    if (h.extension & 2) for (unsigned i = 0; i < 576; ++i) if (!intensity[i]) {
        const auto l = left.spectral[i], r = right.spectral[i];
        left.spectral[i] = (l + r) / std::sqrt(2.0); right.spectral[i] = (l - r) / std::sqrt(2.0);
    }
}

struct Synthesis {
    double overlap[2][576]{};
    double history[2][1024]{};
    unsigned head[2]{};
    void granule(Granule& g, unsigned channel, const Header& h, float* pcm) {
        static const TransformTables t;
        auto& xr = g.spectral;
        const unsigned long_subbands = g.block == 2 ? (g.mixed ? 2 : 0) : 32;
        for (unsigned sb = 1; sb < long_subbands; ++sb) for (unsigned n = 0; n < 8; ++n) {
            const auto high = sb * 18 + n, low = sb * 18 - 1 - n;
            const double u = xr[low], d = xr[high];
            xr[low] = u * t.alias_cos[n] - d * t.alias_sin[n];
            xr[high] = d * t.alias_cos[n] + u * t.alias_sin[n];
        }
        double samples[18][32]{};
        for (unsigned sb = 0; sb < 32; ++sb) {
            double block[36]{};
            if (g.block == 2 && sb >= long_subbands) {
                for (unsigned w = 0; w < 3; ++w) for (unsigned n = 0; n < 12; ++n) for (unsigned k = 0; k < 6; ++k)
                    block[6 + w * 6 + n] += xr[sb * 18 + k * 3 + w] * t.short_imdct[n][k];
            } else {
                const unsigned type = g.block == 2 ? 0 : g.block;
                for (unsigned n = 0; n < 36; ++n) for (unsigned k = 0; k < 18; ++k) block[n] += xr[sb * 18 + k] * t.imdct[type][n][k];
            }
            for (unsigned n = 0; n < 18; ++n) {
                const auto index = sb * 18 + n;
                samples[n][sb] = (block[n] + overlap[channel][index]) * ((sb & n & 1) ? -1 : 1);
                overlap[channel][index] = block[n + 18];
            }
        }
        auto* v = history[channel];
        for (unsigned frame = 0; frame < 18; ++frame) {
            head[channel] = (head[channel] + 960) & 1023;
            const auto offset = head[channel];
            for (unsigned n = 0; n < 64; ++n) {
                double sum = 0;
                for (unsigned k = 0; k < 32; ++k) sum += samples[frame][k] * t.synthesis[n][k];
                v[(offset + n) & 1023] = sum;
            }
            for (unsigned n = 0; n < 32; ++n) {
                double sum = 0;
                for (unsigned k = 0; k < 8; ++k) {
                    sum += v[(offset + k * 128 + n) & 1023] * t.window[k * 64 + n];
                    sum += v[(offset + k * 128 + 96 + n) & 1023] * t.window[k * 64 + 32 + n];
                }
                pcm[(frame * 32 + n) * h.channels + channel] = static_cast<float>(std::clamp(sum, -1.0, 1.0));
            }
        }
    }
};

unsigned crc_update(unsigned crc, unsigned byte) {
    for (unsigned mask = 128; mask; mask >>= 1) {
        const bool bit = ((crc >> 15) ^ ((byte & mask) != 0)) & 1;
        crc = ((crc << 1) & 65535) ^ (bit ? 0x8005 : 0);
    }
    return crc;
}

std::size_t skip_id3(const std::vector<std::uint8_t>& bytes) {
    std::size_t pos = 0;
    for (unsigned tag = 0; tag < 16 && bytes.size() - pos >= 3 && !std::memcmp(bytes.data() + pos, "ID3", 3); ++tag) {
        if (bytes.size() - pos < 10) throw Invalid{};
        const auto* p = bytes.data() + pos;
        if (p[3] < 2 || p[3] > 4 || p[4] == 255 || (p[6] | p[7] | p[8] | p[9]) & 128) throw Invalid{};
        const auto size = (std::size_t(p[6]) << 21) | (std::size_t(p[7]) << 14) | (std::size_t(p[8]) << 7) | p[9];
        const auto total = 10 + size + (p[3] == 4 && (p[5] & 16) ? 10 : 0);
        if (total > bytes.size() - pos) throw Invalid{};
        pos += total;
    }
    return pos;
}

}

Error decode_mp3(const std::vector<std::uint8_t>& bytes, Limits limits, Format& format, std::vector<float>& pcm) {
    try {
        auto pos = skip_id3(bytes);
        if (bytes.size() - pos < 4) return Error::invalid_data;
        Header first;
        if (!header(bytes.data() + pos, first)) return Error::unsupported;
        if (first.channels > limits.channels || first.rate > limits.sample_rate) return Error::limit_exceeded;
        unsigned free_bytes = 0;
        if (!first.bytes) {
            const auto stop = std::min(bytes.size() - 4, pos + 4096);
            for (auto next = pos + 4 + first.side + (first.crc ? 2 : 0); next <= stop; ++next) {
                Header candidate;
                if (header(bytes.data() + next, candidate) && !candidate.bytes && compatible(first, candidate)) {
                    const auto base = static_cast<unsigned>(next - pos) - first.padding;
                    const auto after = next + base + candidate.padding;
                    Header third;
                    if (after == bytes.size() || (after + 4 <= bytes.size() && header(bytes.data() + after, third) && !third.bytes && compatible(first, third))) { free_bytes = base; break; }
                }
            }
            if (!free_bytes) return Error::unsupported;
        }
        Synthesis synthesis;
        std::vector<std::uint8_t> reservoir;
        std::size_t trim_start = 0, trim_end = 0, frames_seen = 0;
        bool gapless = false;
        while (pos < bytes.size()) {
            const auto remaining = bytes.size() - pos;
            if (remaining == 128 && !std::memcmp(bytes.data() + pos, "TAG", 3)) break;
            if (remaining >= 32 && !std::memcmp(bytes.data() + pos, "APETAGEX", 8)) {
                const auto size = u32(bytes.data() + pos + 12);
                if (size >= 32 && size <= remaining && (remaining == size || remaining == size + 32 || remaining == size + 128 || remaining == size + 160)) break;
                return Error::invalid_data;
            }
            if (remaining < 4) return Error::invalid_data;
            Header h;
            const auto* frame = bytes.data() + pos;
            if (!header(frame, h) || !compatible(first, h)) return Error::invalid_data;
            if (!h.bytes) h.bytes = free_bytes ? free_bytes + h.padding : 0;
            if (!h.bytes || h.bytes > remaining) return Error::invalid_data;
            const auto side_offset = 4 + (h.crc ? 2 : 0);
            if (h.bytes < side_offset + h.side) return Error::invalid_data;
            if (h.crc) {
                unsigned crc = crc_update(crc_update(65535, frame[2]), frame[3]);
                for (unsigned n = 0; n < h.side; ++n) crc = crc_update(crc, frame[side_offset + n]);
                if (crc != u16(frame + 4, true)) return Error::invalid_data;
            }
            Granule g[2][2];
            unsigned reuse[2]{}, back = 0;
            Bits side{frame + side_offset,0,h.side * 8};
            side_info(side, h, g, reuse, back);
            const auto start = side_offset + h.side;
            bool metadata = false;
            if (!frames_seen && h.bytes >= 62 && !std::memcmp(frame + 36, "VBRI", 4)) {
                if (u16(frame + 40, true) != 1 || !u32(frame + 50, true)) return Error::invalid_data;
                metadata = true;
            }
            if (!frames_seen && h.bytes - start >= 8 && (!std::memcmp(frame + start, "Xing", 4) || !std::memcmp(frame + start, "Info", 4))) {
                metadata = true;
                auto at = start + 8;
                const auto flags = u32(frame + start + 4, true);
                if (flags & ~15u) return Error::invalid_data;
                at += (flags & 1 ? 4 : 0) + (flags & 2 ? 4 : 0) + (flags & 4 ? 100 : 0) + (flags & 8 ? 4 : 0);
                if (at > h.bytes) return Error::invalid_data;
                if (h.bytes - at >= 24 && (!std::memcmp(frame + at, "LAME", 4) || !std::memcmp(frame + at, "Lavc", 4) || !std::memcmp(frame + at, "Lavf", 4))) {
                    const unsigned delay = (frame[at + 21] << 4) | (frame[at + 22] >> 4);
                    const unsigned padding = ((frame[at + 22] & 15) << 8) | frame[at + 23];
                    if (delay <= 3000 && padding >= 529) { gapless = true; trim_start = delay + 529; trim_end = padding - 529; }
                }
            }
            if (back > reservoir.size()) return Error::invalid_data;
            std::vector<std::uint8_t> main;
            main.insert(main.end(), reservoir.end() - back, reservoir.end());
            main.insert(main.end(), frame + start, frame + h.bytes);
            reservoir.insert(reservoir.end(), frame + start, frame + h.bytes);
            if (reservoir.size() > 511) reservoir.erase(reservoir.begin(), reservoir.end() - 511);
            std::size_t bit = 0;
            const auto& b = bands[h.rate == 8000 ? 6 : h.version == 0 ? 5 : h.rate_index + (h.version == 3 ? 0 : 3)];
            if (!metadata) for (unsigned granule = 0; granule < (h.version == 3 ? 2u : 1u); ++granule) {
                for (unsigned ch = 0; ch < h.channels; ++ch) {
                    auto& v = g[granule][ch];
                    if (bit + v.length > main.size() * 8) return Error::invalid_data;
                    Bits input{main.data(),bit,bit + v.length};
                    scalefactors(input, v, g[0][ch], h, ch, granule ? reuse[ch] : 0);
                    spectrum(input, v, b, h);
                    bit += v.length;
                }
                if (h.channels == 2 && h.extension) stereo(g[granule][0], g[granule][1], h, b);
                const auto count = 576 * h.channels;
                if (pcm.size() > limits.decoded_bytes / sizeof(float) || count > limits.decoded_bytes / sizeof(float) - pcm.size() || pcm.size() > SIZE_MAX - count) return Error::limit_exceeded;
                const auto previous = pcm.size();
                pcm.resize(previous + count);
                for (unsigned ch = 0; ch < h.channels; ++ch) synthesis.granule(g[granule][ch], ch, h, pcm.data() + previous);
            }
            ++frames_seen;
            pos += h.bytes;
        }
        if (!frames_seen || pcm.empty()) return Error::invalid_data;
        if (gapless) {
            if (trim_start + trim_end >= pcm.size() / first.channels) return Error::invalid_data;
            pcm.resize(pcm.size() - trim_end * first.channels);
            pcm.erase(pcm.begin(), pcm.begin() + trim_start * first.channels);
        }
        format.codec = Codec::mp3;
        format.channels = first.channels;
        format.sample_rate = first.rate;
        format.frames = pcm.size() / first.channels;
        format.channel_mask = first.channels == 1 ? 4 : 3;
        return Error::none;
    } catch (const Invalid&) { return Error::invalid_data; }
}

}
