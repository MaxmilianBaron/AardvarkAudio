# AardvarkAudio

C++17 WAV and MP3 decoding. MIT licensed. No external runtime dependencies.

- WAV: PCM 8/16/24/32-bit, float 32/64-bit, A-law, mu-law, IMA and Microsoft ADPCM. RIFF, RIFX, RF64 and extensible PCM.
- MP3: MPEG-1/2/2.5 Layer III, mono/stereo, CBR/VBR, CRC, bit reservoir, joint stereo and Xing/LAME gapless trimming.
- File or memory input, interleaved `float`/`int16_t` output, exact frame seeking and configurable size limits.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Link `AardvarkAudio::aardvark_audio` through `add_subdirectory` or an installed CMake package. Include `<aardvark_audio/audio.h>` and use `aardvark::audio::Reader`. Windows builds support `-A Win32` and `-A x64`; `BUILD_SHARED_LIBS` selects shared libraries.

WAV is converted on demand. MP3 is decoded during `open()` into bounded PCM storage; it is not a streaming API. Defaults: 64 MiB encoded, 256 MiB decoded. Readers own their input and can run independently on different threads; a single reader requires external synchronization.

Layer I/II, damaged-frame concealment, MP3 format changes midstream, RF64 chunk-size tables and partial ADPCM blocks are unsupported. Invalid or excessive input returns an error. Full standards conformance is not certified.

`aardvark_decode input output.wav` converts a file. The optional reference suite generates synthetic audio and compares it with FFmpeg:

```sh
python tests/reference.py build/aardvark_decode
```

On Visual Studio builds use `build/Release/aardvark_decode.exe`. FFmpeg 8.1 with libmp3lame is needed only for reference tests; older versions use different IMA ADPCM rounding. Format constants follow ISO/IEC 11172-3 and ISO/IEC 13818-3; no encoder or playback device is included.
