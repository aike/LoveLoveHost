#include "Lv2Plugin.h"
#include "../util/Log.h"
#include <lv2/atom/util.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/units/units.h>
#include <lv2/parameters/parameters.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

namespace {
constexpr uint32_t kDefaultSeqSize = 32768;

std::string nodeStr(LilvNode* n, const char* fallback = "") {
    std::string s = n ? (lilv_node_is_uri(n) ? lilv_node_as_uri(n) : lilv_node_as_string(n)) : fallback;
    lilv_node_free(n);
    return s;
}
}

Lv2Plugin::Lv2Plugin(Lv2World& world, const LilvPlugin* plugin) : world_(world), plugin_(plugin) {
    uri_ = lilv_node_as_uri(lilv_plugin_get_uri(plugin_));
    name_ = nodeStr(lilv_plugin_get_name(plugin_), uri_.c_str());
    scanPorts();
}

Lv2Plugin::~Lv2Plugin() {
    deactivate();
    if (workerThread_.joinable()) {
        workerQuit_.store(true);
        SetEvent(workerEvent_);
        workerThread_.join();
    }
    if (workerEvent_) CloseHandle(workerEvent_);
    if (instance_) lilv_instance_free(instance_);
}

void Lv2Plugin::scanPorts() {
    auto& N = world_.nodes;
    uint32_t nports = lilv_plugin_get_num_ports(plugin_);
    ports_.resize(nports);
    std::vector<float> defs(nports), mins(nports), maxs(nports);
    lilv_plugin_get_port_ranges_float(plugin_, mins.data(), maxs.data(), defs.data());

    for (uint32_t i = 0; i < nports; ++i) {
        const LilvPort* port = lilv_plugin_get_port_by_index(plugin_, i);
        PortDesc& d = ports_[i];
        d.index = i;
        d.symbol = lilv_node_as_string(lilv_port_get_symbol(plugin_, port));
        d.name = nodeStr(lilv_port_get_name(plugin_, port), d.symbol.c_str());
        d.isInput = lilv_port_is_a(plugin_, port, N.inputPort);
        if (lilv_port_is_a(plugin_, port, N.audioPort)) d.type = PortType::Audio;
        else if (lilv_port_is_a(plugin_, port, N.controlPort)) d.type = PortType::Control;
        else if (lilv_port_is_a(plugin_, port, N.cvPort)) d.type = PortType::CV;
        else if (lilv_port_is_a(plugin_, port, N.atomPort)) d.type = PortType::Atom;

        if (d.type == PortType::Control || d.type == PortType::CV) {
            d.def = std::isnan(defs[i]) ? 0.f : defs[i];
            d.min = std::isnan(mins[i]) ? 0.f : mins[i];
            d.max = std::isnan(maxs[i]) ? 1.f : maxs[i];
            d.toggled = lilv_port_has_property(plugin_, port, N.portToggled);
            d.integer = lilv_port_has_property(plugin_, port, N.portInteger);
            d.enumeration = lilv_port_has_property(plugin_, port, N.portEnumeration);
            d.logarithmic = lilv_port_has_property(plugin_, port, N.portLogarithmic);
            d.sampleRate = lilv_port_has_property(plugin_, port, N.portSampleRate);
            d.notOnGui = lilv_port_has_property(plugin_, port, N.portNotOnGui);
            if (d.toggled) { d.min = 0; d.max = 1; }
            if (d.max < d.min) std::swap(d.min, d.max);
            if (d.def < d.min) d.def = d.min;
            if (d.def > d.max) d.def = d.max;
            // Unit symbol
            LilvNodes* units = lilv_port_get_value(plugin_, port, N.unitsUnit);
            if (units) {
                const LilvNode* unit = lilv_nodes_get_first(units);
                if (unit) {
                    LilvNode* sym = lilv_world_get(world_.world(), unit, N.unitsSymbol, nullptr);
                    if (sym) d.unit = nodeStr(sym);
                    else if (lilv_node_is_uri(unit)) {
                        std::string u = lilv_node_as_uri(unit);
                        size_t p = u.find_last_of("#/");
                        if (p != std::string::npos) d.unit = u.substr(p + 1);
                    }
                }
                lilv_nodes_free(units);
            }
            LilvScalePoints* sps = lilv_port_get_scale_points(plugin_, port);
            if (sps) {
                LILV_FOREACH(scale_points, it, sps) {
                    const LilvScalePoint* sp = lilv_scale_points_get(sps, it);
                    const LilvNode* lab = lilv_scale_point_get_label(sp);
                    const LilvNode* val = lilv_scale_point_get_value(sp);
                    if (lab && val && (lilv_node_is_float(val) || lilv_node_is_int(val)))
                        d.scalePoints.push_back({lilv_node_as_string(lab), lilv_node_as_float(val)});
                }
                lilv_scale_points_free(sps);
                std::sort(d.scalePoints.begin(), d.scalePoints.end(),
                          [](const ScalePoint& a, const ScalePoint& b) { return a.value < b.value; });
            }
        }
        if (d.type == PortType::Atom) {
            d.supportsMidi = lilv_port_supports_event(plugin_, port, N.midiEvent);
            LilvNode* ms = lilv_port_get(plugin_, port, N.rszMinimumSize);
            if (ms && lilv_node_is_int(ms)) d.minimumSize = static_cast<uint32_t>(lilv_node_as_int(ms));
            lilv_node_free(ms);
        }

        switch (d.type) {
        case PortType::Audio:   (d.isInput ? audioIns_ : audioOuts_).push_back(i); break;
        case PortType::Control: (d.isInput ? controlIns_ : controlOuts_).push_back(i); break;
        case PortType::CV:      cvPorts_.push_back(i); break;
        case PortType::Atom:
            (d.isInput ? atomIns_ : atomOuts_).push_back(i);
            if (d.isInput && d.supportsMidi && midiInPort_ < 0) midiInPort_ = static_cast<int>(i);
            break;
        default: break;
        }
    }
    // Control atomics + values
    controlValues_.assign(nports, 0.f);
    controlAtomics_.clear();
    for (uint32_t i = 0; i < nports; ++i) {
        float v = (ports_[i].type == PortType::Control) ? ports_[i].def : 0.f;
        controlValues_[i] = v;
        controlAtomics_.push_back(std::make_unique<std::atomic<float>>(v));
    }
}

int Lv2Plugin::portIndexBySymbol(const char* symbol) const {
    for (auto& p : ports_) if (p.symbol == symbol) return static_cast<int>(p.index);
    return -1;
}

void Lv2Plugin::setControl(uint32_t portIndex, float v) {
    if (portIndex >= controlAtomics_.size()) return;
    controlAtomics_[portIndex]->store(v, std::memory_order_relaxed);
}

void Lv2Plugin::buildFeatures(double sampleRate, uint32_t maxBlock) {
    const HostUrids& U = world_.u();
    featUridMap_   = { LV2_URID__map,   world_.urids().mapFeatureData() };
    featUridUnmap_ = { LV2_URID__unmap, world_.urids().unmapFeatureData() };

    optMinBlock_ = 1;
    optMaxBlock_ = static_cast<int32_t>(maxBlock);
    optNominal_  = static_cast<int32_t>(maxBlock);
    optSeqSize_  = static_cast<int32_t>(kDefaultSeqSize);
    optSampleRate_ = static_cast<float>(sampleRate);
    options_ = {
        { LV2_OPTIONS_INSTANCE, 0, U.bufsz_minBlockLength,     sizeof(int32_t), U.atom_Int,   &optMinBlock_ },
        { LV2_OPTIONS_INSTANCE, 0, U.bufsz_maxBlockLength,     sizeof(int32_t), U.atom_Int,   &optMaxBlock_ },
        { LV2_OPTIONS_INSTANCE, 0, U.bufsz_nominalBlockLength, sizeof(int32_t), U.atom_Int,   &optNominal_ },
        { LV2_OPTIONS_INSTANCE, 0, U.bufsz_sequenceSize,       sizeof(int32_t), U.atom_Int,   &optSeqSize_ },
        { LV2_OPTIONS_INSTANCE, 0, U.param_sampleRate,         sizeof(float),   U.atom_Float, &optSampleRate_ },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr }
    };
    featOptions_ = { LV2_OPTIONS__options, options_.data() };
    featBounded_ = { LV2_BUF_SIZE__boundedBlockLength, nullptr };
    featNominal_ = { LV2_BUF_SIZE__nominalBlockLength, nullptr };
    bool pow2 = maxBlock > 0 && (maxBlock & (maxBlock - 1)) == 0;
    featPow2_ = { LV2_BUF_SIZE__powerOf2BlockLength, nullptr };

    workerSchedule_ = { this, &Lv2Plugin::scheduleWorkCb };
    featWorker_ = { LV2_WORKER__schedule, &workerSchedule_ };

    log_ = { this, &Lv2Plugin::logPrintf, &Lv2Plugin::logVprintf };
    featLog_ = { LV2_LOG__log, &log_ };

    featureList_ = { &featUridMap_, &featUridUnmap_, &featOptions_, &featBounded_, &featNominal_, &featWorker_, &featLog_ };
    if (pow2) featureList_.push_back(&featPow2_);
    featureList_.push_back(nullptr);
}

bool Lv2Plugin::instantiate(double sampleRate, uint32_t maxBlockLength, std::string& error) {
    if (instance_) { error = "already instantiated"; return false; }
    buildFeatures(sampleRate, maxBlockLength);

    // Verify required features
    std::set<std::string> supported;
    for (auto* f : featureList_) if (f) supported.insert(f->URI);
    LilvNodes* req = lilv_plugin_get_required_features(plugin_);
    if (req) {
        LILV_FOREACH(nodes, i, req) {
            const char* uri = lilv_node_as_uri(lilv_nodes_get(req, i));
            if (!supported.count(uri)) {
                error = std::string("Plugin requires unsupported feature: ") + uri;
                lilv_nodes_free(req);
                return false;
            }
        }
        lilv_nodes_free(req);
    }

    instance_ = lilv_plugin_instantiate(plugin_, sampleRate, featureList_.data());
    if (!instance_) { error = "lilv_instance_instantiate failed (see log)"; return false; }
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlockLength;

    // Buffers
    uint32_t nports = static_cast<uint32_t>(ports_.size());
    audioBufs_.assign(nports, {});
    cvBufs_.assign(nports, {});
    atomBufs_.assign(nports, {});
    for (uint32_t i = 0; i < nports; ++i) {
        PortDesc& d = ports_[i];
        switch (d.type) {
        case PortType::Audio:
            audioBufs_[i].assign(maxBlockLength, 0.f);
            lilv_instance_connect_port(instance_, i, audioBufs_[i].data());
            break;
        case PortType::CV:
            cvBufs_[i].assign(maxBlockLength, 0.f);
            lilv_instance_connect_port(instance_, i, cvBufs_[i].data());
            break;
        case PortType::Control:
            if (d.sampleRate && d.isInput) {
                // Ranges are multiples of the sample rate
                float v = d.def * static_cast<float>(sampleRate);
                controlValues_[i] = v;
                controlAtomics_[i]->store(v);
            }
            lilv_instance_connect_port(instance_, i, &controlValues_[i]);
            break;
        case PortType::Atom: {
            uint32_t cap = std::max<uint32_t>(kDefaultSeqSize, d.minimumSize);
            atomBufs_[i].capacity = cap;
            atomBufs_[i].data.assign(cap + sizeof(LV2_Atom_Sequence), 0);
            lilv_instance_connect_port(instance_, i, atomBufs_[i].data.data());
            break;
        }
        default:
            lilv_instance_connect_port(instance_, i, nullptr);
            break;
        }
    }
    atomScratch_.assign(kDefaultSeqSize, 0);

    // Worker
    workerIface_ = static_cast<const LV2_Worker_Interface*>(lilv_instance_get_extension_data(instance_, LV2_WORKER__interface));
    if (workerIface_) {
        workerEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        workerScratch_.assign(65536, 0);
        workerQuit_.store(false);
        workerThread_ = std::thread([this] { workerThread(); });
    }
    return true;
}

void Lv2Plugin::activate() {
    if (instance_ && !active_) { lilv_instance_activate(instance_); active_ = true; }
}
void Lv2Plugin::deactivate() {
    if (instance_ && active_) { lilv_instance_deactivate(instance_); active_ = false; }
}

// ---- Audio thread --------------------------------------------------------

void Lv2Plugin::prepareAtomInputs(uint32_t nframes, RingBuffer* midiIn) {
    const HostUrids& U = world_.u();
    // Reset all atom inputs to empty sequences
    for (uint32_t idx : atomIns_) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomBufs_[idx].data.data());
        seq->atom.type = U.atom_Sequence;
        seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
        seq->body.unit = 0;
        seq->body.pad = 0;
    }
    // MIDI from device
    if (midiIn && midiInPort_ >= 0) {
        auto& ab = atomBufs_[midiInPort_];
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(ab.data.data());
        uint8_t msg[256];
        uint32_t n;
        while ((n = midiIn->readMessage(msg, sizeof(msg))) > 0) {
            uint32_t evSize = static_cast<uint32_t>(sizeof(LV2_Atom_Event)) + n;
            uint32_t padded = lv2_atom_pad_size(evSize);
            if (seq->atom.size + padded > ab.capacity) break;
            auto* ev = reinterpret_cast<LV2_Atom_Event*>(reinterpret_cast<uint8_t*>(seq) + lv2_atom_total_size(&seq->atom));
            ev->time.frames = 0;
            ev->body.type = U.midi_MidiEvent;
            ev->body.size = n;
            memcpy(LV2_ATOM_BODY(&ev->body), msg, n);
            seq->atom.size += padded;
        }
    }
    // Atoms from the plugin UI
    {
        uint32_t n;
        while ((n = uiToPlugin_.readMessage(atomScratch_.data(), static_cast<uint32_t>(atomScratch_.size()))) > 0) {
            if (n < sizeof(uint32_t) + sizeof(LV2_Atom)) continue;
            uint32_t port; memcpy(&port, atomScratch_.data(), sizeof(port));
            if (port >= ports_.size() || ports_[port].type != PortType::Atom || !ports_[port].isInput) continue;
            const LV2_Atom* atom = reinterpret_cast<const LV2_Atom*>(atomScratch_.data() + sizeof(uint32_t));
            auto& ab = atomBufs_[port];
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(ab.data.data());
            uint32_t evSize = static_cast<uint32_t>(sizeof(LV2_Atom_Event)) + atom->size;
            uint32_t padded = lv2_atom_pad_size(evSize);
            if (seq->atom.size + padded > ab.capacity) continue;
            auto* ev = reinterpret_cast<LV2_Atom_Event*>(reinterpret_cast<uint8_t*>(seq) + lv2_atom_total_size(&seq->atom));
            ev->time.frames = 0;
            memcpy(&ev->body, atom, sizeof(LV2_Atom) + atom->size);
            seq->atom.size += padded;
        }
    }
    // Atom outputs: chunk with capacity
    for (uint32_t idx : atomOuts_) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomBufs_[idx].data.data());
        seq->atom.type = U.atom_Chunk;
        seq->atom.size = atomBufs_[idx].capacity;
    }
    (void)nframes;
}

void Lv2Plugin::publishAtomOutputs() {
    if (!forwardAtomOut_.load(std::memory_order_relaxed)) return;
    const HostUrids& U = world_.u();
    for (uint32_t idx : atomOuts_) {
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*>(atomBufs_[idx].data.data());
        if (seq->atom.type != U.atom_Sequence) continue;
        LV2_ATOM_SEQUENCE_FOREACH(seq, ev) {
            uint32_t total = sizeof(uint32_t) + sizeof(LV2_Atom) + ev->body.size;
            if (total > atomScratch_.size()) continue;
            memcpy(atomScratch_.data(), &idx, sizeof(uint32_t));
            memcpy(atomScratch_.data() + sizeof(uint32_t), &ev->body, sizeof(LV2_Atom) + ev->body.size);
            pluginToUi_.writeMessage(atomScratch_.data(), total);
        }
    }
}

void Lv2Plugin::run(const float* const* ins, unsigned nIns, float* const* outs, unsigned nOuts,
                    uint32_t nframes, RingBuffer* midiIn) {
    if (!instance_ || !active_ || nframes > maxBlock_) {
        for (unsigned c = 0; c < nOuts; ++c) memset(outs[c], 0, sizeof(float) * nframes);
        return;
    }
    // Control inputs from GUI
    for (uint32_t idx : controlIns_) controlValues_[idx] = controlAtomics_[idx]->load(std::memory_order_relaxed);

    // Audio inputs: device channel i -> plugin audio in i (extra plugin inputs get silence)
    for (size_t k = 0; k < audioIns_.size(); ++k) {
        uint32_t idx = audioIns_[k];
        if (k < nIns) lilv_instance_connect_port(instance_, idx, const_cast<float*>(ins[k]));
        else {
            memset(audioBufs_[idx].data(), 0, sizeof(float) * nframes);
            lilv_instance_connect_port(instance_, idx, audioBufs_[idx].data());
        }
    }
    // Audio outputs: plugin out j -> device out j
    for (size_t k = 0; k < audioOuts_.size(); ++k) {
        uint32_t idx = audioOuts_[k];
        if (k < nOuts) lilv_instance_connect_port(instance_, idx, outs[k]);
        else lilv_instance_connect_port(instance_, idx, audioBufs_[idx].data());
    }

    prepareAtomInputs(nframes, midiIn);
    lilv_instance_run(instance_, nframes);
    if (workerIface_) {
        handleWorkResponses();
        if (workerIface_->end_run) workerIface_->end_run(handle());
    }

    // Duplicate mono output / silence unused device channels
    if (audioOuts_.size() == 1 && nOuts > 1) {
        for (unsigned c = 1; c < nOuts; ++c) memcpy(outs[c], outs[0], sizeof(float) * nframes);
    } else {
        for (unsigned c = static_cast<unsigned>(audioOuts_.size()); c < nOuts; ++c) memset(outs[c], 0, sizeof(float) * nframes);
    }

    for (uint32_t idx : controlOuts_) controlAtomics_[idx]->store(controlValues_[idx], std::memory_order_relaxed);
    publishAtomOutputs();
}

// ---- UI atom channels ----------------------------------------------------

bool Lv2Plugin::writeAtomToPlugin(uint32_t portIndex, const LV2_Atom* atom) {
    uint32_t total = sizeof(uint32_t) + sizeof(LV2_Atom) + atom->size;
    std::vector<uint8_t> tmp(total);
    memcpy(tmp.data(), &portIndex, sizeof(uint32_t));
    memcpy(tmp.data() + sizeof(uint32_t), atom, sizeof(LV2_Atom) + atom->size);
    return uiToPlugin_.writeMessage(tmp.data(), total);
}

uint32_t Lv2Plugin::readAtomFromPlugin(uint32_t& portIndex, void* buf, uint32_t maxBytes) {
    std::vector<uint8_t> tmp(maxBytes + sizeof(uint32_t));
    uint32_t n = pluginToUi_.readMessage(tmp.data(), static_cast<uint32_t>(tmp.size()));
    if (n <= sizeof(uint32_t)) return 0;
    memcpy(&portIndex, tmp.data(), sizeof(uint32_t));
    memcpy(buf, tmp.data() + sizeof(uint32_t), n - sizeof(uint32_t));
    return n - static_cast<uint32_t>(sizeof(uint32_t));
}

// ---- Worker --------------------------------------------------------------

LV2_Worker_Status Lv2Plugin::scheduleWorkCb(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data) {
    auto* self = static_cast<Lv2Plugin*>(h);
    if (!self->workRequests_.writeMessage(data, size)) return LV2_WORKER_ERR_NO_SPACE;
    SetEvent(self->workerEvent_);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Lv2Plugin::respondCb(LV2_Worker_Respond_Handle h, uint32_t size, const void* data) {
    auto* self = static_cast<Lv2Plugin*>(h);
    return self->workResponses_.writeMessage(data, size) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
}

void Lv2Plugin::workerThread() {
    while (!workerQuit_.load()) {
        WaitForSingleObject(workerEvent_, INFINITE);
        if (workerQuit_.load()) break;
        uint32_t n;
        while ((n = workRequests_.readMessage(workerScratch_.data(), static_cast<uint32_t>(workerScratch_.size()))) > 0) {
            workerIface_->work(handle(), &Lv2Plugin::respondCb, this, n, workerScratch_.data());
        }
    }
}

void Lv2Plugin::handleWorkResponses() {
    if (!workerIface_->work_response) { workResponses_.skip(workResponses_.readAvailable()); return; }
    uint32_t n;
    while ((n = workResponses_.readMessage(atomScratch_.data(), static_cast<uint32_t>(atomScratch_.size()))) > 0) {
        workerIface_->work_response(handle(), n, atomScratch_.data());
    }
}

// ---- Log -----------------------------------------------------------------

int Lv2Plugin::logVprintf(LV2_Log_Handle h, LV2_URID type, const char* fmt, va_list ap) {
    auto* self = static_cast<Lv2Plugin*>(h);
    if (type == self->world_.u().log_Trace) return 0; // too noisy for the audio thread
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    Log::write("[" + self->name_ + "] " + s);
    return n;
}

int Lv2Plugin::logPrintf(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = logVprintf(h, type, fmt, ap);
    va_end(ap);
    return n;
}
