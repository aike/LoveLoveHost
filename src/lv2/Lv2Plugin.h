#pragma once
#include "Lv2World.h"
#include "../util/RingBuffer.h"
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/options/options.h>
#include <lv2/worker/worker.h>
#include <lv2/log/log.h>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

enum class PortType { Unknown, Audio, Control, CV, Atom };

struct ScalePoint { std::string label; float value; };

struct PortDesc {
    uint32_t index = 0;
    std::string symbol, name;
    PortType type = PortType::Unknown;
    bool isInput = false;
    // Control port metadata
    float def = 0, min = 0, max = 1;
    bool toggled = false, integer = false, enumeration = false, logarithmic = false, sampleRate = false, notOnGui = false;
    std::string unit;
    std::vector<ScalePoint> scalePoints;
    // Atom port metadata
    bool supportsMidi = false;
    uint32_t minimumSize = 0;
};

// A single instantiated LV2 plugin with its port buffers, worker thread and
// lock-free channels to the GUI / plugin-UI threads.
class Lv2Plugin {
public:
    Lv2Plugin(Lv2World& world, const LilvPlugin* plugin);
    ~Lv2Plugin();
    Lv2Plugin(const Lv2Plugin&) = delete;
    Lv2Plugin& operator=(const Lv2Plugin&) = delete;

    // Instantiate at the given rate / block size. Returns false with error message on failure.
    bool instantiate(double sampleRate, uint32_t maxBlockLength, std::string& error);
    void activate();
    void deactivate();
    bool isInstantiated() const { return instance_ != nullptr; }

    // Audio-thread processing. `ins`/`outs` are per-channel non-interleaved buffers.
    // midiIn holds length-prefixed raw MIDI messages produced by the MIDI input thread.
    void run(const float* const* ins, unsigned nIns, float* const* outs, unsigned nOuts,
             uint32_t nframes, RingBuffer* midiIn);

    // Metadata
    const LilvPlugin* lilvPlugin() const { return plugin_; }
    const std::string& uri() const { return uri_; }
    const std::string& name() const { return name_; }
    const std::vector<PortDesc>& ports() const { return ports_; }
    const std::vector<uint32_t>& audioInPorts() const { return audioIns_; }
    const std::vector<uint32_t>& audioOutPorts() const { return audioOuts_; }
    const std::vector<uint32_t>& controlInPorts() const { return controlIns_; }
    const std::vector<uint32_t>& controlOutPorts() const { return controlOuts_; }
    int midiInPort() const { return midiInPort_; }
    double sampleRate() const { return sampleRate_; }

    // Control values (thread-safe; GUI / UI side)
    float getControl(uint32_t portIndex) const { return controlAtomics_[portIndex]->load(std::memory_order_relaxed); }
    void  setControl(uint32_t portIndex, float v);
    int   portIndexBySymbol(const char* symbol) const;

    // Atom traffic between plugin UI (non-RT thread) and plugin (RT thread).
    // Message layout: [uint32 portIndex][LV2_Atom ...]
    bool writeAtomToPlugin(uint32_t portIndex, const LV2_Atom* atom);
    uint32_t readAtomFromPlugin(uint32_t& portIndex, void* buf, uint32_t maxBytes); // 0 if none
    void setAtomOutForwarding(bool on) { forwardAtomOut_.store(on); }

    LV2_Handle handle() const { return instance_ ? lilv_instance_get_handle(instance_) : nullptr; }
    const LV2_Descriptor* descriptor() const { return instance_ ? lilv_instance_get_descriptor(instance_) : nullptr; }
    const LV2_Feature* const* features() const { return featureList_.data(); }
    Lv2World& world() { return world_; }

private:
    void scanPorts();
    void buildFeatures(double sampleRate, uint32_t maxBlock);
    void prepareAtomInputs(uint32_t nframes, RingBuffer* midiIn);
    void publishAtomOutputs();

    // Worker
    static LV2_Worker_Status scheduleWorkCb(LV2_Worker_Schedule_Handle h, uint32_t size, const void* data);
    static LV2_Worker_Status respondCb(LV2_Worker_Respond_Handle h, uint32_t size, const void* data);
    void workerThread();
    void handleWorkResponses();

    // Log
    static int logPrintf(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...);
    static int logVprintf(LV2_Log_Handle h, LV2_URID type, const char* fmt, va_list ap);

    Lv2World& world_;
    const LilvPlugin* plugin_;
    std::string uri_, name_;
    LilvInstance* instance_ = nullptr;
    double sampleRate_ = 0;
    uint32_t maxBlock_ = 0;
    bool active_ = false;

    std::vector<PortDesc> ports_;
    std::vector<uint32_t> audioIns_, audioOuts_, controlIns_, controlOuts_, cvPorts_, atomIns_, atomOuts_;
    int midiInPort_ = -1;

    // Port buffers
    std::vector<float> controlValues_;                            // connected to control ports
    std::vector<std::unique_ptr<std::atomic<float>>> controlAtomics_;
    std::vector<std::vector<float>> audioBufs_;                   // per audio port (used for unmapped ports)
    std::vector<std::vector<float>> cvBufs_;
    struct AtomBuf { std::vector<uint8_t> data; uint32_t capacity = 0; };
    std::vector<AtomBuf> atomBufs_;                               // indexed by port index (empty if not atom)

    // Features
    LV2_Feature featUridMap_{}, featUridUnmap_{}, featOptions_{}, featBounded_{}, featPow2_{}, featWorker_{}, featLog_{}, featNominal_{};
    std::vector<LV2_Options_Option> options_;
    LV2_Worker_Schedule workerSchedule_{};
    LV2_Log_Log log_{};
    std::vector<const LV2_Feature*> featureList_;
    int32_t optMinBlock_ = 0, optMaxBlock_ = 0, optNominal_ = 0, optSeqSize_ = 0;
    float optSampleRate_ = 0;

    // Worker
    const LV2_Worker_Interface* workerIface_ = nullptr;
    RingBuffer workRequests_{65536};
    RingBuffer workResponses_{65536};
    std::thread workerThread_;
    HANDLE workerEvent_ = nullptr;
    std::atomic<bool> workerQuit_{false};
    std::vector<uint8_t> workerScratch_;

    // UI <-> plugin atom rings
    RingBuffer uiToPlugin_{65536};
    RingBuffer pluginToUi_{262144};
    std::atomic<bool> forwardAtomOut_{false};
    std::vector<uint8_t> atomScratch_;
};
