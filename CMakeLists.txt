cmake_minimum_required(VERSION 3.21)
project(lovehost VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Everything (exe, DLLs, example plugin bundles) lands in build/bin regardless of configuration.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${cfg} CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${CMAKE_BINARY_DIR}/bin)
endforeach()
option(LOVEHOST_BUILD_EXAMPLES "Build the LV2 example plugins for testing" ON)

find_package(RtAudio CONFIG REQUIRED)
find_package(rtmidi CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# lilv / lv2 ship pkg-config files only; locate them directly.
find_path(LILV_INCLUDE_DIR lilv/lilv.h PATH_SUFFIXES lilv-0 REQUIRED)
find_library(LILV_LIBRARY NAMES lilv-0 lilv REQUIRED)
find_path(LV2_INCLUDE_DIR lv2/core/lv2.h REQUIRED)
# LV2 spec bundles (units, classes, ...) let lilv resolve unit symbols and labels.
find_path(LV2_SPEC_DIR NAMES units.lv2/units.ttl PATH_SUFFIXES lib/lv2)
if(LV2_SPEC_DIR)
    file(COPY ${LV2_SPEC_DIR}/ DESTINATION ${CMAKE_BINARY_DIR}/bin/lv2)
    install(DIRECTORY ${LV2_SPEC_DIR}/ DESTINATION lv2)
endif()
add_library(lilv::lilv UNKNOWN IMPORTED)
set_target_properties(lilv::lilv PROPERTIES
    IMPORTED_LOCATION "${LILV_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${LILV_INCLUDE_DIR};${LV2_INCLUDE_DIR}")

add_executable(lovehost WIN32
    src/main.cpp
    src/util/Config.cpp
    src/util/Log.cpp
    src/lv2/UridMap.cpp
    src/lv2/Lv2World.cpp
    src/lv2/Lv2Plugin.cpp
    src/lv2/Lv2Ui.cpp
    src/audio/AudioEngine.cpp
    src/audio/MidiInput.cpp
    src/gui/MainWindow.cpp
)
set_target_properties(lovehost PROPERTIES OUTPUT_NAME LoveLoveHost)  # user-facing executable name
target_compile_definitions(lovehost PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0A00)
target_compile_options(lovehost PRIVATE /W4 /utf-8 /permissive- /wd4100 /wd4996)
target_link_libraries(lovehost PRIVATE
    lilv::lilv RtAudio::rtaudio RtMidi::rtmidi nlohmann_json::nlohmann_json
    comctl32 ole32 shell32 user32 gdi32)

if(LOVEHOST_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

install(TARGETS lovehost RUNTIME DESTINATION .)
