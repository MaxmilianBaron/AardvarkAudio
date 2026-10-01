#include <aardvark_audio/audio.h>
#include <cstring>

int main() {
    aardvark::audio::Reader reader;
    return reader.is_open() || std::strcmp(aardvark::audio::error_message(reader.error()), "No error");
}
