import argparse
import array
import json
import math
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.decode(errors='replace')[-3000:])
    return result.stdout


def pcm(data):
    result = array.array('h', data)
    if sys.byteorder != 'little':
        result.byteswap()
    return result


def signal(path, rate, channels):
    rng = random.Random(1601)
    samples = array.array('h')
    for i in range(rate):
        t = i / rate
        for c in range(channels):
            phase = t * (440 + 207 * c) + t * t * (1200 + 800 * c)
            value = .3 * math.sin(2 * math.pi * phase) + .08 * math.sin(2 * math.pi * 3301 * t)
            if i % int(rate * .087) < 40:
                value += .4 * (rng.random() - .5)
            if .36 < t < .51:
                value = 0
            samples.append(round(value * 32767))
    if sys.byteorder != 'little':
        samples.byteswap()
    with wave.open(str(path), 'wb') as output:
        output.setparams((channels, 2, rate, 0, 'NONE', 'not compressed'))
        output.writeframes(samples.tobytes())


class Bits:
    def __init__(self):
        self.text = ''

    def put(self, value, width):
        if width:
            assert 0 <= value < 1 << width
            self.text += f'{value:0{width}b}'

    def pack(self):
        text = self.text + '0' * (-len(self.text) % 8)
        return bytes(int(text[i:i + 8], 2) for i in range(0, len(text), 8))


def synthesized(version, rate_index, channels, block=0, mixed=False, extension=0, book=1, entries=None, compress=None, intensity_compress=None):
    tables = Path(__file__).resolve().parents[1] / 'src/mp3_codewords.h'
    words = re.search(r'codewords_' + str(min(book, 24) if book >= 24 else 16 if book >= 16 else book) + r'\[\] = \{(.*?)\}', tables.read_text(), re.S)
    codes = [int(value, 16) for value in re.findall(r'0x[0-9a-f]+', words[1])]
    width = math.isqrt(len(codes))
    entries = list(range(len(codes))) if entries is None else entries
    side, main = Bits(), Bits()
    side.put(0, 9 if version == 3 else 8)
    side.put(0, (5 if channels == 1 else 3) if version == 3 else (1 if channels == 1 else 2))
    if version == 3:
        for _ in range(channels):
            side.put(0, 4)
    linbits = [1,2,3,4,6,8,10,13,4,5,6,7,8,9,11,13]
    for _ in range(2 if version == 3 else 1):
        for channel in range(channels):
            data = Bits()
            intensity = channel == 1 and extension & 1
            scalefac = (5 if version == 3 else 172) if intensity else 0
            scalefac = compress if compress is not None else scalefac
            if intensity and intensity_compress is not None:
                scalefac = intensity_compress
            if version == 3:
                lengths = [(0,0),(0,1),(0,2),(0,3),(3,0),(1,1),(1,2),(1,3),(2,1),(2,2),(2,3),(3,1),(3,2),(3,3),(4,2),(4,3)][scalefac]
                groups = [17 if mixed else 18, 18] if block == 2 else [11, 10]
                for count, length in zip(groups, lengths):
                    for _ in range(count):
                        data.put(1 if intensity and length else 0, length)
            elif intensity:
                c = scalefac >> 1
                if c < 180:
                    lengths = [c // 36, c // 6 % 6, c % 6]
                    counts = [6,15,12] if mixed else [12,12,12] if block == 2 else [7,7,7]
                elif c < 244:
                    c -= 180
                    lengths = [c // 16, c // 4 % 4, c % 4]
                    counts = [6,12,9,6] if mixed else [12,9,9,6] if block == 2 else [6,6,6,3]
                    lengths.append(0)
                else:
                    c -= 244
                    lengths = [c // 3, c % 3, 0]
                    counts = [6,18,9] if mixed else [15,12,9] if block == 2 else [8,8,5]
                for count, length in zip(counts, lengths):
                    for _ in range(count):
                        data.put(1 if length > 1 else 0, length)
            else:
                counts = [[6,5,5,5],[9,9,9,9],[6,9,9,9]]
                group = 2 if mixed else 1 if block == 2 else 0
                if scalefac < 400:
                    lengths = [(scalefac >> 4) // 5, (scalefac >> 4) % 5, (scalefac >> 2) % 4, scalefac % 4]
                elif scalefac < 500:
                    c = scalefac - 400
                    counts = [[6,5,7,3],[9,9,12,6],[6,9,12,6]]
                    lengths = [(c >> 2) // 5, (c >> 2) % 5, c % 4, 0]
                else:
                    c = scalefac - 500
                    counts = [[11,10,0,0],[18,18,0,0],[15,18,0,0]]
                    lengths = [c // 3, c % 3, 0, 0]
                for count, length in zip(counts[group], lengths):
                    for _ in range(count):
                        data.put(0, length)
            values = [1] if intensity else entries
            for index in values:
                code = codes[index]
                length = code.bit_length() - 1
                data.put(code - (1 << length), length)
                for value in divmod(index, width):
                    if value == 15 and book >= 16:
                        data.put(1, linbits[book - 16])
                    if value:
                        data.put(index & 1, 1)
            side.put(len(data.text), 12)
            side.put(len(values), 9)
            side.put(175, 8)
            side.put(scalefac, 4 if version == 3 else 9)
            side.put(bool(block), 1)
            if block:
                side.put(block, 2); side.put(mixed, 1)
                side.put(book, 5); side.put(book, 5)
                for _ in range(3):
                    side.put(0, 3)
            else:
                for _ in range(3):
                    side.put(book, 5)
                side.put(0, 4); side.put(0, 3)
            if version == 3:
                side.put(0, 1)
            side.put(0, 1); side.put(0, 1)
            main.text += data.text
    header = bytes([255, 0xe3 | version << 3, (0xe0 if version == 3 else 0xc0) | rate_index << 2, 0xc0 if channels == 1 else (0x40 | extension << 4) if extension else 0])
    rate = [44100,48000,32000][rate_index] >> (0 if version == 3 else 1 if version == 2 else 2)
    length = (144000 * 320 if version == 3 else 72000 * 128) // rate
    frame = header + side.pack() + main.pack()
    assert len(frame) <= length
    return (frame + bytes(length - len(frame))) * 6


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('decoder', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--filter', default='')
    args = parser.parse_args()
    decoder = args.decoder.resolve()
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise SystemExit('FFmpeg with libmp3lame is required for reference tests')
    report = []
    with tempfile.TemporaryDirectory(prefix='aardvark-audio-') as directory:
        work = Path(directory)

        def compare(source, tolerance=1, fact_frames=None):
            if args.filter and args.filter not in source.name:
                return
            out = source.with_suffix('.decoded.wav')
            run([decoder, source, out])
            reference = pcm(run([ffmpeg, '-v', 'error', '-i', source, '-f', 's16le', '-']))
            with wave.open(str(out), 'rb') as stream:
                actual = pcm(stream.readframes(stream.getnframes()))
                channels = stream.getnchannels()
            if fact_frames is not None:
                reference = reference[:fact_frames * channels]
            assert len(actual) == len(reference), (source.name, len(actual), len(reference))
            error = max((abs(a - b) for a, b in zip(actual, reference)), default=0)
            item = dict(file=source.name, samples=len(actual), max_error=error)
            report.append(item)
            print(json.dumps(item), flush=True)
            assert error <= tolerance, item

        for rate in (44100,48000,32000,22050,24000,16000,11025,12000,8000):
            for channels in (1, 2):
                source = work / 'signal.wav'
                signal(source, rate, channels)
                for mode in ('cbr', 'vbr'):
                    target = work / f'{rate}-{channels}-{mode}.mp3'
                    encoding = ['-b:a', '128k' if rate >= 32000 else '48k'] if mode == 'cbr' else ['-q:a', '4']
                    run([ffmpeg, '-v', 'error', '-y', '-i', source, '-codec:a', 'libmp3lame', *encoding, target])
                    compare(target)
        for channels in (1,2):
            source = work / 'signal.wav'
            signal(source, 44100, channels)
            for codec in ('pcm_u8','pcm_s16le','pcm_s24le','pcm_s32le','pcm_f32le','pcm_f64le','pcm_alaw','pcm_mulaw','adpcm_ima_wav','adpcm_ms'):
                target = work / f'{codec}-{channels}.wav'
                run([ffmpeg, '-v', 'error', '-y', '-i', source, '-codec:a', codec, target])
                compare(target, 1, 44100 if codec.startswith('adpcm_') else None)
        for book in (1,2,3,5,6,7,8,9,10,11,12,13,15,*range(16,32)):
            target = work / f'codebook-{book}.mp3'
            target.write_bytes(synthesized(3,0,1,book=book))
            compare(target)
        audio = synthesized(3,0,2,entries=[1,2,3]*30)
        metadata = bytearray(synthesized(3,0,2,entries=[])[:1044])
        metadata[36:62] = b'VBRI' + struct.pack('>HHHIIHHHH',1,0,0,len(audio)+1044,6,0,0,0,0)
        target = work / 'metadata-vbri.mp3'
        target.write_bytes(metadata + audio)
        compare(target)
        for version in (3,2,0):
            for rate_index in range(3):
                for block, mixed in ((0,False),(1,False),(2,False),(2,True),(3,False)):
                    for extension in (0,1,2,3):
                        target = work / f'blocks-{version}-{rate_index}-{block}-{mixed}-{extension}.mp3'
                        target.write_bytes(synthesized(version,rate_index,2,block,mixed,extension,entries=[1,2,3]*30))
                        compare(target)
        for version, values in ((3,range(16)),(2,range(512))):
            for compress in values:
                target = work / f'scale-{version}-{compress}.mp3'
                target.write_bytes(synthesized(version,0,1,compress=compress,entries=[1,2,3]*10))
                compare(target)
        for compress in (0,1,2,5,172,359,360,361,487,488,510,511):
            for block, mixed in ((0,False),(2,False),(2,True)):
                for extension in (1,3):
                    target = work / f'intensity-{compress}-{block}-{mixed}-{extension}.mp3'
                    target.write_bytes(synthesized(2,0,2,block,mixed,extension,entries=[1,2,3]*30,intensity_compress=compress))
                    compare(target)
    if args.output:
        args.output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(f'{len(report)} reference comparisons passed')


if __name__ == '__main__':
    main()
