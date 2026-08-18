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

find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(QUIC_BRIDGE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/quic-bridge/Cargo.toml")
set(QUIC_BRIDGE_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo-target")
set(QUIC_BRIDGE_DLL "${QUIC_BRIDGE_TARGET_DIR}/release/livesuite_audio_quic.dll")

add_custom_target(livesuite_audio_quic_bridge ALL
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${QUIC_BRIDGE_TARGET_DIR}"
        ${CARGO_EXECUTABLE} build --release --manifest-path "${QUIC_BRIDGE_MANIFEST}"
    BYPRODUCTS "${QUIC_BRIDGE_DLL}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/quic-bridge"
    COMMENT "Building LiveSuite audio QUIC bridge"
    VERBATIM
)

add_executable(audio_server_udp
    main.cpp
    wasapi_renderer.cpp
    opus_decoder.cpp
    quic_audio_receiver.cpp
)

add_dependencies(audio_server_udp livesuite_audio_quic_bridge)

add_custom_command(TARGET audio_server_udp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${QUIC_BRIDGE_DLL}"
        "$<TARGET_FILE_DIR:audio_server_udp>/livesuite_audio_quic.dll"
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

install(TARGETS audio_server_udp
    RUNTIME DESTINATION .
    COMPONENT runtime
)

install(FILES "${QUIC_BRIDGE_DLL}"
    DESTINATION .
    COMPONENT runtime
)

install(FILES THIRD_PARTY_NOTICES.md
    DESTINATION .
    COMPONENT runtime
)
