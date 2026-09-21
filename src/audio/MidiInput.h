#pragma once
#include "../util/RingBuffer.h"
#include <rtmidi/RtMidi.h>
#include <memory>
#include <string>
#include <vector>

// Forwards raw MIDI messages from an RtMidi input port into a ring buffer
// consumed by the audio thread.
class MidiInput {
public:
    explicit MidiInput(RingBuffer& ring);
    ~MidiInput();

    std::vector<std::string> ports() const;
    bool open(const std::string& portName, std::string& error);
    void close();
    bool isOpen() const { return open_; }
    const std::string& portName() const { return portName_; }

private:
    static void callback(double dt, std::vector<unsigned char>* msg, void* user);
    RingBuffer& ring_;
    std::unique_ptr<RtMidiIn> in_;
    bool open_ = false;
    std::string portName_;
};
