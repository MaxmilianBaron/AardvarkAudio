cmake_minimum_required(VERSION 3.20)
project(AardvarkAudio VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
option(AARDVARK_AUDIO_EXAMPLES "Build the decoding utility" ON)
option(AARDVARK_AUDIO_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

add_library(aardvark_audio src/audio.cpp src/wave.cpp src/mp3.cpp)
add_library(AardvarkAudio::aardvark_audio ALIAS aardvark_audio)
target_compile_features(aardvark_audio PUBLIC cxx_std_17)
target_include_directories(aardvark_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(aardvark_audio PROPERTIES CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)

function(aardvark_audio_checks target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    if(AARDVARK_AUDIO_SANITIZERS)
        if(MSVC)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
        else()
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()
aardvark_audio_checks(aardvark_audio)

if(AARDVARK_AUDIO_EXAMPLES)
    add_executable(aardvark_decode examples/decode.cpp)
    target_link_libraries(aardvark_decode PRIVATE aardvark_audio)
    aardvark_audio_checks(aardvark_decode)
endif()

if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(aardvark_audio_tests tests/wave_tests.cpp)
    target_link_libraries(aardvark_audio_tests PRIVATE aardvark_audio)
    aardvark_audio_checks(aardvark_audio_tests)
    add_test(NAME audio COMMAND aardvark_audio_tests)
    add_executable(aardvark_mp3_tests tests/mp3_tests.cpp)
    target_link_libraries(aardvark_mp3_tests PRIVATE aardvark_audio Threads::Threads)
    aardvark_audio_checks(aardvark_mp3_tests)
    add_test(NAME mp3 COMMAND aardvark_mp3_tests)
endif()

install(TARGETS aardvark_audio EXPORT AardvarkAudioTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT AardvarkAudioTargets FILE AardvarkAudioTargets.cmake
    NAMESPACE AardvarkAudio:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AardvarkAudio)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/AardvarkAudioConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/AardvarkAudioConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AardvarkAudio)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/AardvarkAudioConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AardvarkAudioConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/AardvarkAudioConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/AardvarkAudio)
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/AardvarkAudio)
