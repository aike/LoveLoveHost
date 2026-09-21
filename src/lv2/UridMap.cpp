#include "UridMap.h"
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/parameters/parameters.h>
#include <lv2/ui/ui.h>
#include <lv2/log/log.h>

UridMap::UridMap() {
    toUri_.push_back(""); // URID 0 is reserved / invalid
    mapData_.handle = this;
    mapData_.map = &UridMap::mapCb;
    unmapData_.handle = this;
    unmapData_.unmap = &UridMap::unmapCb;
}

LV2_URID UridMap::map(const char* uri) {
    if (!uri) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = toId_.find(uri);
    if (it != toId_.end()) return it->second;
    LV2_URID id = static_cast<LV2_URID>(toUri_.size());
    toUri_.emplace_back(uri);
    toId_.emplace(uri, id);
    return id;
}

const char* UridMap::unmap(LV2_URID urid) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (urid == 0 || urid >= toUri_.size()) return nullptr;
    return toUri_[urid].c_str();
}

LV2_URID UridMap::mapCb(LV2_URID_Map_Handle h, const char* uri) {
    return static_cast<UridMap*>(h)->map(uri);
}
const char* UridMap::unmapCb(LV2_URID_Unmap_Handle h, LV2_URID urid) {
    return static_cast<UridMap*>(h)->unmap(urid);
}

void HostUrids::init(UridMap& m) {
    atom_Chunk = m.map(LV2_ATOM__Chunk);
    atom_Sequence = m.map(LV2_ATOM__Sequence);
    atom_Float = m.map(LV2_ATOM__Float);
    atom_Int = m.map(LV2_ATOM__Int);
    atom_Long = m.map(LV2_ATOM__Long);
    atom_Double = m.map(LV2_ATOM__Double);
    atom_Bool = m.map(LV2_ATOM__Bool);
    atom_eventTransfer = m.map(LV2_ATOM__eventTransfer);
    midi_MidiEvent = m.map(LV2_MIDI__MidiEvent);
    bufsz_minBlockLength = m.map(LV2_BUF_SIZE__minBlockLength);
    bufsz_maxBlockLength = m.map(LV2_BUF_SIZE__maxBlockLength);
    bufsz_nominalBlockLength = m.map(LV2_BUF_SIZE__nominalBlockLength);
    bufsz_sequenceSize = m.map(LV2_BUF_SIZE__sequenceSize);
    param_sampleRate = m.map(LV2_PARAMETERS__sampleRate);
    ui_updateRate = m.map(LV2_UI__updateRate);
    ui_scaleFactor = m.map(LV2_UI__scaleFactor);
    log_Error = m.map(LV2_LOG__Error);
    log_Note = m.map(LV2_LOG__Note);
    log_Trace = m.map(LV2_LOG__Trace);
    log_Warning = m.map(LV2_LOG__Warning);
}
