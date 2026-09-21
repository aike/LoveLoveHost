#pragma once
#include <string>
#include <vector>

// Persistent host settings (device selection, plugin choice). Plugin state is not saved.
struct Config {
    std::string audioApi = "ASIO";     // RtAudio API name
    std::string outputDevice;          // device name ("" = default)
    std::string inputDevice;           // device name ("" = same as output for ASIO)
    unsigned    sampleRate = 48000;
    unsigned    bufferSize = 256;
    std::string midiDevice;            // RtMidi port name ("" = none)
    std::string pluginUri;             // last loaded LV2 plugin URI
    std::vector<std::string> extraLv2Paths;
    bool        showPluginUi = false;
    int         windowX = -1, windowY = -1;

    static std::string defaultPath();  // %APPDATA%\LoveLoveHost\config.json
    bool load(const std::string& path = defaultPath());
    bool save(const std::string& path = defaultPath()) const;
};
