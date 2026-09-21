#include "MidiInput.h"
#include "../util/Log.h"

MidiInput::MidiInput(RingBuffer& ring) : ring_(ring) {
    try {
        in_ = std::make_unique<RtMidiIn>(RtMidi::WINDOWS_MM, "LoveLoveHost");
    } catch (RtMidiError& e) {
        Log::write(std::string("[RtMidi] ") + e.getMessage());
    }
}

MidiInput::~MidiInput() { close(); }

std::vector<std::string> MidiInput::ports() const {
    std::vector<std::string> names;
    if (!in_) return names;
    unsigned n = in_->getPortCount();
    for (unsigned i = 0; i < n; ++i) {
        try { names.push_back(in_->getPortName(i)); } catch (RtMidiError&) {}
    }
    return names;
}

bool MidiInput::open(const std::string& portName, std::string& error) {
    close();
    if (!in_) { error = "RtMidi not available"; return false; }
    unsigned n = in_->getPortCount();
    for (unsigned i = 0; i < n; ++i) {
        if (in_->getPortName(i) != portName) continue;
        try {
            in_->openPort(i, "LoveLoveHost in");
            in_->ignoreTypes(true, true, true); // no sysex / timing / active sensing
            in_->setCallback(&MidiInput::callback, this);
            open_ = true;
            portName_ = portName;
            Log::write("MIDI input opened: " + portName);
            return true;
        } catch (RtMidiError& e) {
            error = e.getMessage();
            return false;
        }
    }
    error = "MIDI port not found: " + portName;
    return false;
}

void MidiInput::close() {
    if (in_ && open_) {
        in_->cancelCallback();
        in_->closePort();
    }
    open_ = false;
    portName_.clear();
}

void MidiInput::callback(double, std::vector<unsigned char>* msg, void* user) {
    auto* self = static_cast<MidiInput*>(user);
    if (!msg || msg->empty() || msg->size() > 255) return;
    self->ring_.writeMessage(msg->data(), static_cast<uint32_t>(msg->size()));
}
