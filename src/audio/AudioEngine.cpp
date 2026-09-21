#include "AudioEngine.h"
#include "../lv2/Lv2Plugin.h"
#include "../util/Log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {
void rtErrorCb(RtAudioErrorType type, const std::string& text) {
    if (type == RTAUDIO_WARNING) Log::write("[RtAudio warning] " + text);
    else Log::write("[RtAudio error] " + text);
}
}

AudioEngine::AudioEngine() {
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    RtAudio::Api pick = apis.empty() ? RtAudio::UNSPECIFIED : apis.front();
    for (auto a : apis) if (a == RtAudio::WINDOWS_ASIO) pick = a;
    rt_ = std::make_unique<RtAudio>(pick, &rtErrorCb);
    apiName_ = RtAudio::getApiDisplayName(pick);
}

AudioEngine::~AudioEngine() { stop(); }

std::vector<std::string> AudioEngine::apiNames() {
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    std::vector<std::string> names;
    for (auto a : apis) names.push_back(RtAudio::getApiDisplayName(a));
    return names;
}

bool AudioEngine::selectApi(const std::string& name) {
    std::vector<RtAudio::Api> apis;
    RtAudio::getCompiledApi(apis);
    for (auto a : apis) {
        if (RtAudio::getApiDisplayName(a) == name) {
            stop();
            rt_ = std::make_unique<RtAudio>(a, &rtErrorCb);
            apiName_ = name;
            return true;
        }
    }
    return false;
}

std::vector<AudioDeviceInfo> AudioEngine::devices() const {
    std::vector<AudioDeviceInfo> out;
    for (unsigned id : rt_->getDeviceIds()) {
        RtAudio::DeviceInfo di = rt_->getDeviceInfo(id);
        AudioDeviceInfo d;
        d.id = di.ID;
        d.name = di.name;
        d.inputs = di.inputChannels;
        d.outputs = di.outputChannels;
        d.sampleRates = di.sampleRates;
        d.preferredSampleRate = di.preferredSampleRate;
        out.push_back(std::move(d));
    }
    return out;
}

bool AudioEngine::start(unsigned outputDeviceId, unsigned inputDeviceId, unsigned sampleRate,
                        unsigned bufferFrames, unsigned wantInputs, unsigned wantOutputs, std::string& error) {
    stop();
    RtAudio::DeviceInfo outInfo = rt_->getDeviceInfo(outputDeviceId);
    if (outInfo.outputChannels == 0) { error = "Output device has no output channels"; return false; }
    if (rt_->getCurrentApi() == RtAudio::WINDOWS_ASIO) inputDeviceId = outputDeviceId; // ASIO: one device for both
    RtAudio::DeviceInfo inInfo = rt_->getDeviceInfo(inputDeviceId);

    nOut_ = std::min<unsigned>(outInfo.outputChannels, std::max<unsigned>(wantOutputs, 1));
    nIn_ = std::min<unsigned>(inInfo.inputChannels, wantInputs);

    RtAudio::StreamParameters outP{outputDeviceId, nOut_, 0};
    RtAudio::StreamParameters inP{inputDeviceId, nIn_, 0};
    RtAudio::StreamOptions opts;
    opts.flags = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME;
    opts.streamName = "LoveLoveHost";
    unsigned frames = bufferFrames;
    RtAudioErrorType err = rt_->openStream(&outP, nIn_ > 0 ? &inP : nullptr, RTAUDIO_FLOAT32, sampleRate,
                                           &frames, &AudioEngine::callback, this, &opts);
    if (err != RTAUDIO_NO_ERROR) { error = "openStream: " + rt_->getErrorText(); return false; }
    bufferFrames_ = frames;
    sampleRate_ = rt_->getStreamSampleRate();
    inPtrs_.assign(nIn_, nullptr);
    outPtrs_.assign(nOut_, nullptr);
    xruns_.store(0);
    err = rt_->startStream();
    if (err != RTAUDIO_NO_ERROR) {
        error = "startStream: " + rt_->getErrorText();
        rt_->closeStream();
        return false;
    }
    running_ = true;
    Log::write("Audio started: " + apiName_ + " / " + outInfo.name + " @ " + std::to_string(sampleRate_) + " Hz, " +
               std::to_string(bufferFrames_) + " frames, in=" + std::to_string(nIn_) + " out=" + std::to_string(nOut_) +
               ", latency=" + std::to_string(rt_->getStreamLatency()) + " frames");
    return true;
}

void AudioEngine::stop() {
    if (!rt_) return;
    if (rt_->isStreamRunning()) rt_->stopStream();
    if (rt_->isStreamOpen()) rt_->closeStream();
    running_ = false;
}

int AudioEngine::callback(void* out, void* in, unsigned nFrames, double, RtAudioStreamStatus st, void* user) {
    return static_cast<AudioEngine*>(user)->process(static_cast<float*>(out), static_cast<const float*>(in), nFrames, st);
}

int AudioEngine::process(float* out, const float* in, unsigned nFrames, RtAudioStreamStatus st) {
    auto t0 = std::chrono::steady_clock::now();
    if (st) xruns_.fetch_add(1, std::memory_order_relaxed);
    for (unsigned c = 0; c < nOut_; ++c) outPtrs_[c] = out + c * nFrames;
    for (unsigned c = 0; c < nIn_; ++c) inPtrs_[c] = in ? in + c * nFrames : nullptr;

    Lv2Plugin* p = plugin_.load(std::memory_order_acquire);
    if (p) {
        p->run(inPtrs_.data(), in ? nIn_ : 0, outPtrs_.data(), nOut_, nFrames, &midi_);
    } else {
        // Pass-through (or silence when there is no input)
        for (unsigned c = 0; c < nOut_; ++c) {
            if (in && c < nIn_) memcpy(outPtrs_[c], inPtrs_[c], sizeof(float) * nFrames);
            else memset(outPtrs_[c], 0, sizeof(float) * nFrames);
        }
        midi_.skip(midi_.readAvailable());
    }

    float peak = 0.f;
    for (unsigned c = 0; c < nOut_; ++c)
        for (unsigned i = 0; i < nFrames; ++i) {
            float v = outPtrs_[c][i];
            if (!std::isfinite(v)) { v = 0.f; outPtrs_[c][i] = 0.f; }
            peak = std::max(peak, std::fabs(v));
        }
    peak_.store(std::max(peak, peak_.load(std::memory_order_relaxed) * 0.9f), std::memory_order_relaxed);

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double budget = sampleRate_ ? static_cast<double>(nFrames) / sampleRate_ : 1.0;
    cpu_.store(cpu_.load(std::memory_order_relaxed) * 0.9 + (elapsed / budget) * 0.1, std::memory_order_relaxed);
    return 0;
}
