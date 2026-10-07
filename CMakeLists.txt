cmake_minimum_required(VERSION 3.20)
project(AardvarkAudio VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

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
endfunction()
aardvark_audio_checks(aardvark_audio)



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
