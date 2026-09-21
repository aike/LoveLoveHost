#pragma once
#include <string>
#include <functional>

// Simple log sink; the GUI attaches a callback to show messages in a status area.
namespace Log {
    void set(std::function<void(const std::string&)> sink);
    void openFile(const std::string& path);   // also mirror every line into this file
    void write(const std::string& msg);
}
