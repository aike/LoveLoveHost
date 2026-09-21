#pragma once
#include "../util/RingBuffer.h"
#include <rtaudio/RtAudio.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Lv2Plugin;

struct AudioDeviceInfo {
    unsigned id = 0;
    std::string name;
    unsigned inputs = 0, outputs = 0;
    std::vector<unsigned> sampleRates;
    unsigned preferredSampleRate = 0;
};

// Wraps RtAudio. The plugin is swapped only while the stream is stopped, so the
// audio callback never races with plugin (re)construction.
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    static std::vector<std::string> apiNames();          // e.g. "ASIO", "WASAPI"
    bool selectApi(const std::string& name);             // recreates the RtAudio object
    const std::string& apiName() const { return apiName_; }
    std::vector<AudioDeviceInfo> devices() const;

    // Opens and starts the stream. `bufferFrames` is a request; the driver may adjust it.
    bool start(unsigned outputDeviceId, unsigned inputDeviceId, unsigned sampleRate,
               unsigned bufferFrames, unsigned wantInputs, unsigned wantOutputs, std::string& error);
    void stop();
    bool isRunning() const { return running_; }
    unsigned actualBufferFrames() const { return bufferFrames_; }
    unsigned actualSampleRate() const { return sampleRate_; }
    unsigned inputChannels() const { return nIn_; }
    unsigned outputChannels() const { return nOut_; }

    // Setting a plugin while running is safe (the callback picks it up next block).
    // Clearing / destroying a plugin must only happen while the stream is stopped.
    void setPlugin(Lv2Plugin* p) { plugin_.store(p, std::memory_order_release); }
    Lv2Plugin* plugin() const { return plugin_.load(std::memory_order_acquire); }

    RingBuffer& midiRing() { return midi_; }
    float peakOut() const { return peak_.load(std::memory_order_relaxed); }
    unsigned xruns() const { return xruns_.load(std::memory_order_relaxed); }
    double cpuLoad() const { return cpu_.load(std::memory_order_relaxed); }

private:
    static int callback(void* out, void* in, unsigned nFrames, double t, RtAudioStreamStatus st, void* user);
    int process(float* out, const float* in, unsigned nFrames, RtAudioStreamStatus st);

    std::unique_ptr<RtAudio> rt_;
    std::string apiName_;
    bool running_ = false;
    unsigned bufferFrames_ = 0, sampleRate_ = 0, nIn_ = 0, nOut_ = 0;
    std::atomic<Lv2Plugin*> plugin_{nullptr};
    RingBuffer midi_{16384};
    std::vector<const float*> inPtrs_;
    std::vector<float*> outPtrs_;
    std::atomic<float> peak_{0.f};
    std::atomic<unsigned> xruns_{0};
    std::atomic<double> cpu_{0.0};
    std::string lastError_;
};
