cmake_minimum_required(VERSION 3.24)

project(audio_server_udp LANGUAGES C CXX)

include(FetchContent)

set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
)
FetchContent_MakeAvailable(opus)

add_executable(audio_server_udp
    main.cpp
    wasapi_renderer.cpp
    opus_decoder.cpp
)

target_compile_features(audio_server_udp PRIVATE cxx_std_17)
target_compile_definitions(audio_server_udp PRIVATE
    _UNICODE
    UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(audio_server_udp PRIVATE /EHsc /O2 /utf-8 /W3)
endif()

target_link_libraries(audio_server_udp PRIVATE
    opus
    ws2_32
    ole32
    avrt
    uuid
)
