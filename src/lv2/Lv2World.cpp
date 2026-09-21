#include "Lv2World.h"
#include "../util/Log.h"
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/ui/ui.h>
#include <lv2/units/units.h>
#include <algorithm>
#include <cstdlib>
#include <windows.h>

namespace {
std::string exeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    size_t pos = w.find_last_of(L"\\/");
    if (pos != std::wstring::npos) w = w.substr(0, pos);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len - 1), ' ');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

std::string envUtf8(const char* name) {
    char* v = nullptr; size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) { out = v; free(v); }
    return out;
}
}

Lv2World::Lv2World(const std::vector<std::string>& extraPaths) {
    urids_.init(uridMap_);
    init(extraPaths);
}

Lv2World::~Lv2World() {
    freeNodes();
    lilv_world_free(world_);
}

void Lv2World::freeNodes() {
    LilvNode** first = reinterpret_cast<LilvNode**>(&nodes);
    size_t count = sizeof(nodes) / sizeof(LilvNode*);
    for (size_t i = 0; i < count; ++i) { lilv_node_free(first[i]); first[i] = nullptr; }
}

void Lv2World::rescan(const std::vector<std::string>& extraPaths) {
    freeNodes();
    lilv_world_free(world_);
    world_ = nullptr;
    init(extraPaths);
}

void Lv2World::init(const std::vector<std::string>& extraPaths) {
    world_ = lilv_world_new();
    auto N = [&](const char* uri) { return lilv_new_uri(world_, uri); };
    nodes.audioPort = N(LV2_CORE__AudioPort);
    nodes.controlPort = N(LV2_CORE__ControlPort);
    nodes.cvPort = N(LV2_CORE__CVPort);
    nodes.atomPort = N(LV2_ATOM__AtomPort);
    nodes.inputPort = N(LV2_CORE__InputPort);
    nodes.outputPort = N(LV2_CORE__OutputPort);
    nodes.midiEvent = N(LV2_MIDI__MidiEvent);
    nodes.atomBufferType = N(LV2_ATOM__bufferType);
    nodes.atomSequence = N(LV2_ATOM__Sequence);
    nodes.portToggled = N(LV2_CORE__toggled);
    nodes.portInteger = N(LV2_CORE__integer);
    nodes.portEnumeration = N(LV2_CORE__enumeration);
    nodes.portLogarithmic = N(LV2_PORT_PROPS__logarithmic);
    nodes.portSampleRate = N(LV2_CORE__sampleRate);
    nodes.portNotOnGui = N(LV2_PORT_PROPS__notOnGUI);
    nodes.rszMinimumSize = N(LV2_RESIZE_PORT__minimumSize);
    nodes.windowsUi = N(LV2_UI__WindowsUI);
    nodes.uiShowInterface = N(LV2_UI__showInterface);
    nodes.uiIdleInterface = N(LV2_UI__idleInterface);
    nodes.unitsUnit = N(LV2_UNITS__unit);
    nodes.unitsSymbol = N(LV2_UNITS__symbol);
    nodes.unitsRender = N(LV2_UNITS__render);
    nodes.rdfsLabel = N("http://www.w3.org/2000/01/rdf-schema#label");

    // Build LV2_PATH: extra paths, exe-relative "lv2", then user / common dirs.
    std::string path;
    auto add = [&](const std::string& p) { if (p.empty()) return; if (!path.empty()) path += ";"; path += p; };
    for (auto& p : extraPaths) add(p);
    add(exeDir() + "\\lv2");
    std::string existing = envUtf8("LV2_PATH");
    if (!existing.empty()) add(existing);
    else {
        add(envUtf8("APPDATA") + "\\LV2");
        add(envUtf8("COMMONPROGRAMFILES") + "\\LV2");
    }
    LilvNode* pathNode = lilv_new_string(world_, path.c_str());
    lilv_world_set_option(world_, LILV_OPTION_LV2_PATH, pathNode);
    lilv_node_free(pathNode);
    Log::write("LV2_PATH = " + path);

    lilv_world_load_all(world_);

    plugins_.clear();
    const LilvPlugins* all = lilv_world_get_all_plugins(world_);
    LILV_FOREACH(plugins, i, all) {
        const LilvPlugin* p = lilv_plugins_get(all, i);
        PluginInfo info;
        info.uri = lilv_node_as_uri(lilv_plugin_get_uri(p));
        LilvNode* name = lilv_plugin_get_name(p);
        info.name = name ? lilv_node_as_string(name) : info.uri;
        lilv_node_free(name);
        const LilvNode* bundle = lilv_plugin_get_bundle_uri(p);
        if (bundle) {
            char* bp = lilv_file_uri_parse(lilv_node_as_uri(bundle), nullptr);
            if (bp) { info.bundlePath = bp; lilv_free(bp); }
        }
        uint32_t nports = lilv_plugin_get_num_ports(p);
        for (uint32_t k = 0; k < nports; ++k) {
            const LilvPort* port = lilv_plugin_get_port_by_index(p, k);
            bool in = lilv_port_is_a(p, port, nodes.inputPort);
            if (lilv_port_is_a(p, port, nodes.audioPort)) {
                if (in) info.audioIns++; else info.audioOuts++;
            } else if (in && lilv_port_is_a(p, port, nodes.atomPort) && lilv_port_supports_event(p, port, nodes.midiEvent)) {
                info.hasMidiIn = true;
            }
        }
        LilvUIs* uis = lilv_plugin_get_uis(p);
        if (uis) {
            LILV_FOREACH(uis, ui, uis) {
                const LilvUI* u = lilv_uis_get(uis, ui);
                if (lilv_ui_is_a(u, nodes.windowsUi)) { info.hasWindowsUi = true; break; }
            }
            lilv_uis_free(uis);
        }
        plugins_.push_back(std::move(info));
    }
    std::sort(plugins_.begin(), plugins_.end(), [](const PluginInfo& a, const PluginInfo& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    Log::write("Found " + std::to_string(plugins_.size()) + " LV2 plugin(s)");
}

const LilvPlugin* Lv2World::findPlugin(const std::string& uri) const {
    LilvNode* n = lilv_new_uri(world_, uri.c_str());
    const LilvPlugin* p = lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world_), n);
    lilv_node_free(n);
    return p;
}
