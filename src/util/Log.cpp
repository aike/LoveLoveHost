#include "Log.h"
#include <mutex>
#include <cstdio>
#include <ctime>

namespace {
    std::mutex g_mutex;
    std::function<void(const std::string&)> g_sink;
    FILE* g_file = nullptr;
}

void Log::set(std::function<void(const std::string&)> sink) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sink = std::move(sink);
}

void Log::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) fclose(g_file);
    g_file = fopen(path.c_str(), "w");
}

void Log::write(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(stderr, "%s\n", msg.c_str());
    if (g_file) {
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_s(&tmv, &t);
        char ts[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(g_file, "%s %s\n", ts, msg.c_str());
        fflush(g_file);
    }
    if (g_sink) g_sink(msg);
}
