#pragma once
#include <lilv/lilv.h>
#include <string>
#include <vector>
#include "UridMap.h"

struct PluginInfo {
    std::string uri;
    std::string name;
    std::string bundlePath;
    unsigned audioIns = 0, audioOuts = 0;
    bool hasMidiIn = false;
    bool hasWindowsUi = false;
};

// Owns the lilv world and plugin discovery. One instance per process.
class Lv2World {
public:
    explicit Lv2World(const std::vector<std::string>& extraPaths);
    ~Lv2World();
    Lv2World(const Lv2World&) = delete;
    Lv2World& operator=(const Lv2World&) = delete;

    // Rebuilds the lilv world (lilv cannot unload bundles). URIDs stay valid.
    void rescan(const std::vector<std::string>& extraPaths);
    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    const LilvPlugin* findPlugin(const std::string& uri) const;

    LilvWorld* world() const { return world_; }
    UridMap& urids() { return uridMap_; }
    const HostUrids& u() const { return urids_; }

    // Frequently used nodes
    struct Nodes {
        LilvNode *audioPort, *controlPort, *cvPort, *atomPort, *inputPort, *outputPort;
        LilvNode *midiEvent, *atomBufferType, *atomSequence;
        LilvNode *portToggled, *portInteger, *portEnumeration, *portLogarithmic, *portSampleRate, *portNotOnGui;
        LilvNode *rszMinimumSize;
        LilvNode *windowsUi, *uiShowInterface, *uiIdleInterface;
        LilvNode *unitsUnit, *unitsSymbol, *unitsRender, *rdfsLabel;
    } nodes{};

private:
    void init(const std::vector<std::string>& extraPaths);
    void freeNodes();

    LilvWorld* world_ = nullptr;
    UridMap uridMap_;
    HostUrids urids_{};
    std::vector<PluginInfo> plugins_;
};
