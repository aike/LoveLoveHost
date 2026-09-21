#pragma once
#include <lv2/urid/urid.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Thread-safe URI <-> integer mapping shared by plugin, UI and host.
class UridMap {
public:
    UridMap();
    LV2_URID map(const char* uri);
    const char* unmap(LV2_URID urid);

    LV2_URID_Map*   mapFeatureData()   { return &mapData_; }
    LV2_URID_Unmap* unmapFeatureData() { return &unmapData_; }

private:
    static LV2_URID    mapCb(LV2_URID_Map_Handle h, const char* uri);
    static const char* unmapCb(LV2_URID_Unmap_Handle h, LV2_URID urid);

    std::mutex mutex_;
    std::unordered_map<std::string, LV2_URID> toId_;
    std::vector<std::string> toUri_;
    LV2_URID_Map   mapData_;
    LV2_URID_Unmap unmapData_;
};

// URIDs the host needs frequently.
struct HostUrids {
    LV2_URID atom_Chunk, atom_Sequence, atom_Float, atom_Int, atom_Long, atom_Double, atom_Bool, atom_eventTransfer;
    LV2_URID midi_MidiEvent;
    LV2_URID bufsz_minBlockLength, bufsz_maxBlockLength, bufsz_nominalBlockLength, bufsz_sequenceSize;
    LV2_URID param_sampleRate;
    LV2_URID ui_updateRate, ui_scaleFactor;
    LV2_URID log_Error, log_Note, log_Trace, log_Warning;
    void init(UridMap& m);
};
