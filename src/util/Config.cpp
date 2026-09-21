#include "Config.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <filesystem>
#include <cstdlib>

using json = nlohmann::json;

std::string Config::defaultPath() {
    char* appdata = nullptr;
    size_t len = 0;
    std::string base;
    if (_dupenv_s(&appdata, &len, "APPDATA") == 0 && appdata) {
        base = appdata;
        free(appdata);
    } else {
        base = ".";
    }
    return base + "\\LoveLoveHost\\config.json";
}

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    try {
        json j = json::parse(f);
        audioApi     = j.value("audioApi", audioApi);
        outputDevice = j.value("outputDevice", outputDevice);
        inputDevice  = j.value("inputDevice", inputDevice);
        sampleRate   = j.value("sampleRate", sampleRate);
        bufferSize   = j.value("bufferSize", bufferSize);
        midiDevice   = j.value("midiDevice", midiDevice);
        pluginUri    = j.value("pluginUri", pluginUri);
        showPluginUi = j.value("showPluginUi", showPluginUi);
        windowX      = j.value("windowX", windowX);
        windowY      = j.value("windowY", windowY);
        extraLv2Paths = j.value("extraLv2Paths", extraLv2Paths);
        return true;
    } catch (...) {
        return false;
    }
}

bool Config::save(const std::string& path) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    json j;
    j["audioApi"]     = audioApi;
    j["outputDevice"] = outputDevice;
    j["inputDevice"]  = inputDevice;
    j["sampleRate"]   = sampleRate;
    j["bufferSize"]   = bufferSize;
    j["midiDevice"]   = midiDevice;
    j["pluginUri"]    = pluginUri;
    j["showPluginUi"] = showPluginUi;
    j["windowX"]      = windowX;
    j["windowY"]      = windowY;
    j["extraLv2Paths"] = extraLv2Paths;
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2);
    return true;
}
