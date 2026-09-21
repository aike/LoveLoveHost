#pragma once
#include "Lv2Plugin.h"
#include <lv2/ui/ui.h>
#include <lv2/data-access/data-access.h>
#include <lv2/instance-access/instance-access.h>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// Hosts a plugin's ui:WindowsUI inside a parent HWND supplied by the GUI.
// All methods must be called from the GUI thread.
class Lv2Ui {
public:
    explicit Lv2Ui(Lv2Plugin& plugin);
    ~Lv2Ui();
    Lv2Ui(const Lv2Ui&) = delete;
    Lv2Ui& operator=(const Lv2Ui&) = delete;

    static bool hasWindowsUi(Lv2World& world, const LilvPlugin* plugin);

    bool open(HWND parent, std::string& error);
    void close();
    bool isOpen() const { return handle_ != nullptr; }
    HWND widget() const { return widget_; }

    // Call at ~30 Hz from a GUI timer. Returns false when the UI asked to be closed.
    bool idle();

    // Host -> UI notifications
    void notifyControl(uint32_t port, float value);

    // Callbacks to the host GUI
    std::function<void(int w, int h)> onResize;
    std::function<void(uint32_t port, float value)> onControlFromUi;

private:
    static void     writeFn(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t protocol, const void* buf);
    static int      resizeFn(LV2UI_Feature_Handle h, int w, int height);
    static uint32_t portIndexFn(LV2UI_Feature_Handle h, const char* symbol);
    static uint32_t subscribeFn(LV2UI_Feature_Handle h, uint32_t port, uint32_t protocol, const LV2_Feature* const* f);
    static uint32_t unsubscribeFn(LV2UI_Feature_Handle h, uint32_t port, uint32_t protocol, const LV2_Feature* const* f);

    bool findUi(std::string& error);
    void sendInitialValues();
    void pumpControlOutputs();
    void pumpAtomOutputs();

    Lv2Plugin& plugin_;
    std::string uiUri_, binaryPath_, bundlePath_;
    HMODULE module_ = nullptr;
    const LV2UI_Descriptor* descriptor_ = nullptr;
    LV2UI_Handle handle_ = nullptr;
    HWND widget_ = nullptr;
    HWND parent_ = nullptr;
    const LV2UI_Idle_Interface* idleIface_ = nullptr;

    // Features
    LV2_Feature featMap_{}, featUnmap_{}, featParent_{}, featResize_{}, featInstance_{}, featData_{}, featIdle_{},
                featPortMap_{}, featSubscribe_{}, featOptions_{}, featLog_{};
    LV2UI_Resize resize_{};
    LV2UI_Port_Map portMap_{};
    LV2UI_Port_Subscribe subscribe_{};
    LV2_Extension_Data_Feature dataAccess_{};
    std::vector<LV2_Options_Option> options_;
    float optUpdateRate_ = 30.f, optScale_ = 1.f, optSampleRate_ = 48000.f;
    LV2_Log_Log log_{};
    std::vector<const LV2_Feature*> features_;

    std::vector<float> lastOut_; // last control-output value sent to the UI (by port index)
    std::vector<uint8_t> atomBuf_;
    bool inWrite_ = false;
};
