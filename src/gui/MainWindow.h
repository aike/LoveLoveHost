#pragma once
#include "../audio/AudioEngine.h"
#include "../audio/MidiInput.h"
#include "../lv2/Lv2World.h"
#include "../lv2/Lv2Plugin.h"
#include "../lv2/Lv2Ui.h"
#include "../util/Config.h"
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

// Main application window: device / plugin selection, generic control panel,
// log view, and an owned window that embeds the plugin's native UI.
class MainWindow {
public:
    // autoExitSeconds > 0 closes the window after that many seconds (for scripted smoke tests).
    MainWindow(HINSTANCE hInst, Config& config, const std::string& initialPluginUri, int autoExitSeconds = 0);
    ~MainWindow();
    bool create();
    HWND hwnd() const { return hwnd_; }

private:
    // Window procs
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK panelProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK uiWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    LRESULT handlePanel(UINT, WPARAM, LPARAM);
    LRESULT handleUiWnd(HWND, UINT, WPARAM, LPARAM);

    // Layout / widgets
    void createWidgets();
    void layout();
    void populateApis();
    void populateDevices();
    void populateSampleRates();
    void populateBufferSizes();
    void populateMidiPorts();
    void populatePlugins();
    void rebuildPanel();
    void updateStatus();
    void appendLog(const std::string& line);
    int  s(int v) const { return MulDiv(v, dpi_, 96); } // DPI scale

    // Actions
    void startAudio();
    void stopAudio();
    bool instantiatePlugin();
    void loadSelectedPlugin();
    void loadPluginByUri(const std::string& uri);
    void unloadPlugin();
    void rescanPlugins();
    void togglePluginUi();
    void openPluginUi();
    void closePluginUi();
    void saveConfig();

    // Control panel helpers
    struct Row {
        uint32_t port = 0;
        HWND label = nullptr, control = nullptr, value = nullptr;
        enum Kind { Slider, Toggle, Enum, Output } kind = Slider;
        int steps = 1000;
    };
    float sliderToValue(const Row& r, int pos) const;
    int   valueToSlider(const Row& r, float v) const;
    void  setRowValue(Row& r, float v, bool fromUi);
    void  refreshRowDisplay(Row& r, float v);
    std::string formatValue(const PortDesc& d, float v) const;

    HINSTANCE hInst_;
    Config& config_;
    std::string initialPluginUri_;
    int autoExitSeconds_ = 0;
    HWND hwnd_ = nullptr, panel_ = nullptr, log_ = nullptr, uiWnd_ = nullptr;
    HWND cbApi_, cbDevice_, cbRate_, cbBuffer_, btnStart_, cbMidi_, cbPlugin_, btnLoad_, btnRescan_, btnUi_, status_;
    HWND lblApi_, lblDevice_, lblRate_, lblBuffer_, lblMidi_, lblPlugin_;
    HFONT font_ = nullptr;
    int dpi_ = 96;
    int panelContentHeight_ = 0, panelScroll_ = 0;
    bool updatingControls_ = false;

    std::unique_ptr<Lv2World> world_;
    AudioEngine engine_;
    MidiInput midi_;
    std::unique_ptr<Lv2Plugin> plugin_;
    std::unique_ptr<Lv2Ui> ui_;
    std::vector<AudioDeviceInfo> devices_;
    std::vector<std::string> midiPorts_;
    std::vector<Row> rows_;
};
