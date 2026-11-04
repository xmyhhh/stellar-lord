cmake_minimum_required(VERSION 3.20)

project(orbital_freight_taskbar VERSION 0.1.0 LANGUAGES CXX)

find_program(POWERSHELL_EXECUTABLE NAMES powershell pwsh REQUIRED)

set(APP_LOGO_PNG "${CMAKE_CURRENT_SOURCE_DIR}/logo.png")
set(APP_ICON_ICO "${CMAKE_CURRENT_BINARY_DIR}/app_icon.ico")
set(RESOURCE_HEADER_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h")

file(TO_CMAKE_PATH "${APP_LOGO_PNG}" APP_LOGO_PNG_PATH)
file(TO_CMAKE_PATH "${APP_ICON_ICO}" APP_ICON_ICO_PATH)
file(TO_CMAKE_PATH "${RESOURCE_HEADER_PATH}" RESOURCE_HEADER_PATH)

add_custom_command(
    OUTPUT "${APP_ICON_ICO}"
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.ps1"
            -InputPng "${APP_LOGO_PNG}"
            -OutputIco "${APP_ICON_ICO}"
    DEPENDS
        "${APP_LOGO_PNG}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_app_icon.ps1"
    COMMENT "Generating application icon from logo.png"
    VERBATIM
)

add_custom_target(generate_app_icon DEPENDS "${APP_ICON_ICO}")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app_generated.rc"
    @ONLY
)

add_executable(orbital_freight_taskbar WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/app_generated.rc"
    src/app_config.cpp
    src/game_model.cpp
    src/main.cpp
    src/net_client.cpp
    src/process_monitor.cpp
    src/system_metrics.cpp
    src/taskbar_embedder.cpp
)

add_dependencies(orbital_freight_taskbar generate_app_icon)

target_compile_features(orbital_freight_taskbar PRIVATE cxx_std_20)

target_compile_definitions(orbital_freight_taskbar PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
)

target_link_libraries(orbital_freight_taskbar PRIVATE
    pdh
    iphlpapi
    psapi
    advapi32
    shell32
    user32
    ws2_32
)

if(MSVC)
    target_compile_options(orbital_freight_taskbar PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GF /Gy /Gw>
    )
    target_link_options(orbital_freight_taskbar PRIVATE
        $<$<CONFIG:Release>:/OPT:REF /OPT:ICF /INCREMENTAL:NO>
    )
endif()

# 联机服务端：零外部依赖的单 exe HTTP 服务，房间全部驻留内存。
add_executable(freight_server
    server/main.cpp
    src/game_model.cpp
)
target_include_directories(freight_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(freight_server PRIVATE cxx_std_20)
target_compile_definitions(freight_server PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A000000
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(freight_server PRIVATE
    ws2_32
)
if(MSVC)
    target_compile_options(freight_server PRIVATE
        $<$<CONFIG:Release>:/O2 /Oi /Ot /GF /Gy /Gw>
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(game_model_tests
        tests/game_model_tests.cpp
        src/game_model.cpp
    )
    target_include_directories(game_model_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_features(game_model_tests PRIVATE cxx_std_20)
    target_compile_options(game_model_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/EHsc>)
    target_compile_definitions(game_model_tests PRIVATE UNICODE _UNICODE NOMINMAX)
    set_target_properties(game_model_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests"
    )
    add_test(NAME game_model COMMAND game_model_tests)
endif()
