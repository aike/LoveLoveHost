#include "MainWindow.h"
#include "../util/Log.h"
#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr int ID_API = 101, ID_DEVICE = 102, ID_RATE = 103, ID_BUFFER = 104, ID_START = 105, ID_MIDI = 106,
              ID_PLUGIN = 107, ID_LOAD = 108, ID_RESCAN = 109, ID_UI = 110;
constexpr int ID_ROW_BASE = 1000;
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_AUTOSTART = WM_APP + 2;
constexpr UINT_PTR TIMER_STATUS = 1, TIMER_UI = 2, TIMER_EXIT = 3;
constexpr int SLIDER_STEPS = 1000;
constexpr unsigned kBufferSizes[] = {32, 64, 128, 256, 512, 1024, 2048};

std::wstring toW(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len > 0 ? len - 1 : 0), L' ');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}
std::string toU8(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), ' ');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}
void setText(HWND h, const std::string& s) { SetWindowTextW(h, toW(s).c_str()); }
int comboAdd(HWND cb, const std::string& s) { return static_cast<int>(SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(toW(s).c_str()))); }
int comboSel(HWND cb) { return static_cast<int>(SendMessageW(cb, CB_GETCURSEL, 0, 0)); }
void comboSetSel(HWND cb, int i) { SendMessageW(cb, CB_SETCURSEL, i, 0); }
std::string comboText(HWND cb) {
    int i = comboSel(cb);
    if (i < 0) return "";
    int len = static_cast<int>(SendMessageW(cb, CB_GETLBTEXTLEN, i, 0));
    std::wstring w(static_cast<size_t>(len), L' ');
    SendMessageW(cb, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(w.data()));
    return toU8(w);
}
}

MainWindow::MainWindow(HINSTANCE hInst, Config& config, const std::string& initialPluginUri, int autoExitSeconds)
    : hInst_(hInst), config_(config), initialPluginUri_(initialPluginUri), autoExitSeconds_(autoExitSeconds),
      midi_(engine_.midiRing()) {}

MainWindow::~MainWindow() {
    Log::set(nullptr);
    closePluginUi();
    engine_.stop();
    plugin_.reset();
    if (font_) DeleteObject(font_);
}

// ---- creation -------------------------------------------------------------

bool MainWindow::create() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = &MainWindow::wndProc;
    wc.hInstance = hInst_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"LovehostMain";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassW(&wc);
    WNDCLASSW pc = wc;
    pc.lpfnWndProc = &MainWindow::panelProc;
    pc.lpszClassName = L"LovehostPanel";
    RegisterClassW(&pc);
    WNDCLASSW uc = wc;
    uc.lpfnWndProc = &MainWindow::uiWndProc;
    uc.lpszClassName = L"LovehostPluginUi";
    RegisterClassW(&uc);

    int x = config_.windowX >= 0 ? config_.windowX : CW_USEDEFAULT;
    int y = config_.windowY >= 0 ? config_.windowY : CW_USEDEFAULT;
    UINT sysDpi = GetDpiForSystem();
    hwnd_ = CreateWindowExW(0, L"LovehostMain", L"LoveLoveHost - LV2 host", WS_OVERLAPPEDWINDOW,
                            x, y, MulDiv(820, sysDpi, 96), MulDiv(640, sysDpi, 96), nullptr, nullptr, hInst_, this);
    if (!hwnd_) return false;
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK MainWindow::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    MainWindow* self;
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK MainWindow::panelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->panel_ = h; // handlePanel needs it before CreateWindowExW returns
    }
    return self ? self->handlePanel(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK MainWindow::uiWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->handleUiWnd(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

void MainWindow::createWidgets() {
    dpi_ = static_cast<int>(GetDpiForWindow(hwnd_));
    font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hwnd_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        return h;
    };
    const DWORD cbs = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP;
    lblApi_ = mk(L"STATIC", L"API", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbApi_ = mk(L"COMBOBOX", L"", cbs, ID_API);
    lblDevice_ = mk(L"STATIC", L"Device", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbDevice_ = mk(L"COMBOBOX", L"", cbs, ID_DEVICE);
    lblRate_ = mk(L"STATIC", L"Rate", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbRate_ = mk(L"COMBOBOX", L"", cbs, ID_RATE);
    lblBuffer_ = mk(L"STATIC", L"Buffer", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbBuffer_ = mk(L"COMBOBOX", L"", cbs, ID_BUFFER);
    btnStart_ = mk(L"BUTTON", L"Start", BS_PUSHBUTTON | WS_TABSTOP, ID_START);
    lblMidi_ = mk(L"STATIC", L"MIDI", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbMidi_ = mk(L"COMBOBOX", L"", cbs, ID_MIDI);
    lblPlugin_ = mk(L"STATIC", L"Plugin", SS_RIGHT | SS_CENTERIMAGE, 0);
    cbPlugin_ = mk(L"COMBOBOX", L"", cbs, ID_PLUGIN);
    btnLoad_ = mk(L"BUTTON", L"Load", BS_PUSHBUTTON | WS_TABSTOP, ID_LOAD);
    btnRescan_ = mk(L"BUTTON", L"Rescan", BS_PUSHBUTTON | WS_TABSTOP, ID_RESCAN);
    btnUi_ = mk(L"BUTTON", L"Show UI", BS_PUSHBUTTON | WS_TABSTOP, ID_UI);
    status_ = mk(L"STATIC", L"Stopped", SS_LEFT | SS_CENTERIMAGE, 0);
    panel_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LovehostPanel", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                             0, 0, 10, 10, hwnd_, nullptr, hInst_, this);
    log_ = mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, WS_EX_CLIENTEDGE);
    SendMessageW(log_, EM_SETLIMITTEXT, 0, 0);
    EnableWindow(btnUi_, FALSE);

    Log::set([this](const std::string& line) {
        PostMessageW(hwnd_, WM_APP_LOG, 0, reinterpret_cast<LPARAM>(new std::string(line)));
    });
}

void MainWindow::layout() {
    RECT rc; GetClientRect(hwnd_, &rc);
    int W = rc.right, H = rc.bottom;
    auto place = [&](HWND h, int x, int y, int w, int hh) { MoveWindow(h, s(x), s(y), s(w), s(hh), TRUE); };
    auto placeCombo = [&](HWND h, int x, int y, int w) { MoveWindow(h, s(x), s(y), s(w), s(200), TRUE); };
    int y = 10;
    place(lblApi_, 5, y, 40, 22);       placeCombo(cbApi_, 50, y, 110);
    place(lblDevice_, 165, y, 50, 22);  placeCombo(cbDevice_, 220, y, 230);
    place(lblRate_, 455, y, 40, 22);    placeCombo(cbRate_, 500, y, 75);
    place(lblBuffer_, 580, y, 45, 22);  placeCombo(cbBuffer_, 630, y, 70);
    place(btnStart_, 710, y, 80, 24);
    y = 42;
    place(lblMidi_, 5, y, 40, 22);      placeCombo(cbMidi_, 50, y, 180);
    place(lblPlugin_, 235, y, 50, 22);  placeCombo(cbPlugin_, 290, y, 285);
    place(btnLoad_, 580, y, 55, 24);
    place(btnRescan_, 640, y, 65, 24);
    place(btnUi_, 710, y, 80, 24);
    y = 74;
    MoveWindow(status_, s(8), s(y), W - s(16), s(20), TRUE);
    int logH = s(110);
    int panelTop = s(100);
    int panelH = std::max(s(50), H - panelTop - logH - s(10));
    MoveWindow(panel_, s(8), panelTop, W - s(16), panelH, TRUE);
    MoveWindow(log_, s(8), panelTop + panelH + s(5), W - s(16), logH, TRUE);
    // Panel rows
    RECT prc; GetClientRect(panel_, &prc);
    int pw = prc.right;
    for (size_t i = 0; i < rows_.size(); ++i) {
        Row& r = rows_[i];
        int ry = s(4) + static_cast<int>(i) * s(30) - panelScroll_;
        int labelW = std::min(s(220), pw / 3);
        int valueW = s(120);
        int ctrlX = labelW + s(10);
        int ctrlW = std::max(s(60), pw - ctrlX - valueW - s(10));
        MoveWindow(r.label, s(4), ry, labelW, s(24), TRUE);
        if (r.kind == Row::Toggle) MoveWindow(r.control, ctrlX, ry, s(24), s(24), TRUE);
        else if (r.kind == Row::Enum) MoveWindow(r.control, ctrlX, ry, ctrlW, s(200), TRUE);
        else if (r.control) MoveWindow(r.control, ctrlX, ry, ctrlW, s(26), TRUE);
        if (r.value) MoveWindow(r.value, ctrlX + ctrlW + s(5), ry, valueW, s(24), TRUE);
    }
    panelContentHeight_ = s(8) + static_cast<int>(rows_.size()) * s(30);
    SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
    si.nMin = 0; si.nMax = std::max(0, panelContentHeight_ - 1); si.nPage = static_cast<UINT>(prc.bottom); si.nPos = panelScroll_;
    SetScrollInfo(panel_, SB_VERT, &si, TRUE);
}

// ---- combo population -----------------------------------------------------

void MainWindow::populateApis() {
    SendMessageW(cbApi_, CB_RESETCONTENT, 0, 0);
    int sel = 0, i = 0;
    for (auto& n : AudioEngine::apiNames()) {
        comboAdd(cbApi_, n);
        if (n == config_.audioApi) sel = i;
        ++i;
    }
    comboSetSel(cbApi_, sel);
    engine_.selectApi(comboText(cbApi_));
}

void MainWindow::populateDevices() {
    SendMessageW(cbDevice_, CB_RESETCONTENT, 0, 0);
    devices_.clear();
    int sel = 0, i = 0;
    for (auto& d : engine_.devices()) {
        if (d.outputs == 0) continue;
        comboAdd(cbDevice_, d.name + "  (" + std::to_string(d.inputs) + " in / " + std::to_string(d.outputs) + " out)");
        if (d.name == config_.outputDevice) sel = i;
        devices_.push_back(d);
        ++i;
    }
    comboSetSel(cbDevice_, sel);
    populateSampleRates();
}

void MainWindow::populateSampleRates() {
    SendMessageW(cbRate_, CB_RESETCONTENT, 0, 0);
    int di = comboSel(cbDevice_);
    std::vector<unsigned> rates;
    unsigned pref = 48000;
    if (di >= 0 && di < static_cast<int>(devices_.size())) { rates = devices_[di].sampleRates; pref = devices_[di].preferredSampleRate; }
    if (rates.empty()) rates = {44100, 48000, 88200, 96000};
    int sel = -1, i = 0;
    for (unsigned r : rates) {
        comboAdd(cbRate_, std::to_string(r));
        if (r == config_.sampleRate) sel = i;
        if (sel < 0 && r == pref) sel = i;
        ++i;
    }
    if (sel < 0) sel = 0;
    comboSetSel(cbRate_, sel);
}

void MainWindow::populateBufferSizes() {
    SendMessageW(cbBuffer_, CB_RESETCONTENT, 0, 0);
    int sel = 3, i = 0;
    for (unsigned b : kBufferSizes) {
        comboAdd(cbBuffer_, std::to_string(b));
        if (b == config_.bufferSize) sel = i;
        ++i;
    }
    comboSetSel(cbBuffer_, sel);
}

void MainWindow::populateMidiPorts() {
    SendMessageW(cbMidi_, CB_RESETCONTENT, 0, 0);
    midiPorts_ = midi_.ports();
    comboAdd(cbMidi_, "(none)");
    int sel = 0, i = 1;
    for (auto& p : midiPorts_) {
        comboAdd(cbMidi_, p);
        if (p == config_.midiDevice) sel = i;
        ++i;
    }
    comboSetSel(cbMidi_, sel);
}

void MainWindow::populatePlugins() {
    SendMessageW(cbPlugin_, CB_RESETCONTENT, 0, 0);
    comboAdd(cbPlugin_, "(none)");
    int sel = 0, i = 1;
    std::string want = plugin_ ? plugin_->uri() : (initialPluginUri_.empty() ? config_.pluginUri : initialPluginUri_);
    for (auto& p : world_->plugins()) {
        std::string tag = std::to_string(p.audioIns) + "in/" + std::to_string(p.audioOuts) + "out";
        if (p.hasMidiIn) tag += ", MIDI";
        if (p.hasWindowsUi) tag += ", UI";
        comboAdd(cbPlugin_, p.name + "  [" + tag + "]");
        if (p.uri == want) sel = i;
        ++i;
    }
    comboSetSel(cbPlugin_, sel);
}

// ---- generic control panel -------------------------------------------------

std::string MainWindow::formatValue(const PortDesc& d, float v) const {
    char buf[64];
    if (d.integer || d.toggled) snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    else if (std::fabs(v) >= 1000.f) snprintf(buf, sizeof(buf), "%.0f", v);
    else if (std::fabs(v) >= 10.f) snprintf(buf, sizeof(buf), "%.1f", v);
    else snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    if (!d.unit.empty()) s += " " + d.unit;
    return s;
}

float MainWindow::sliderToValue(const Row& r, int pos) const {
    const PortDesc& d = plugin_->ports()[r.port];
    float t = static_cast<float>(pos) / static_cast<float>(r.steps);
    float v;
    if (d.logarithmic && d.min > 0.f && d.max > d.min) v = d.min * std::pow(d.max / d.min, t);
    else v = d.min + (d.max - d.min) * t;
    if (d.integer) v = std::round(v);
    return std::clamp(v, d.min, d.max);
}

int MainWindow::valueToSlider(const Row& r, float v) const {
    const PortDesc& d = plugin_->ports()[r.port];
    float t;
    if (d.logarithmic && d.min > 0.f && d.max > d.min && v > 0.f) t = std::log(v / d.min) / std::log(d.max / d.min);
    else t = (d.max > d.min) ? (v - d.min) / (d.max - d.min) : 0.f;
    return static_cast<int>(std::lround(std::clamp(t, 0.f, 1.f) * static_cast<float>(r.steps)));
}

void MainWindow::refreshRowDisplay(Row& r, float v) {
    const PortDesc& d = plugin_->ports()[r.port];
    updatingControls_ = true;
    switch (r.kind) {
    case Row::Slider:
        SendMessageW(r.control, TBM_SETPOS, TRUE, valueToSlider(r, v));
        setText(r.value, formatValue(d, v));
        break;
    case Row::Toggle:
        SendMessageW(r.control, BM_SETCHECK, v > 0.5f ? BST_CHECKED : BST_UNCHECKED, 0);
        setText(r.value, v > 0.5f ? "on" : "off");
        break;
    case Row::Enum: {
        int best = 0; float bestDist = 1e30f;
        for (size_t i = 0; i < d.scalePoints.size(); ++i) {
            float dist = std::fabs(d.scalePoints[i].value - v);
            if (dist < bestDist) { bestDist = dist; best = static_cast<int>(i); }
        }
        comboSetSel(r.control, best);
        setText(r.value, formatValue(d, v));
        break;
    }
    case Row::Output:
        setText(r.value, formatValue(d, v));
        break;
    }
    updatingControls_ = false;
}

void MainWindow::setRowValue(Row& r, float v, bool fromUi) {
    if (!plugin_) return;
    plugin_->setControl(r.port, v);
    refreshRowDisplay(r, v);
    if (!fromUi && ui_) ui_->notifyControl(r.port, v);
}

void MainWindow::rebuildPanel() {
    for (auto& r : rows_) {
        if (r.label) DestroyWindow(r.label);
        if (r.control) DestroyWindow(r.control);
        if (r.value) DestroyWindow(r.value);
    }
    rows_.clear();
    panelScroll_ = 0;
    if (plugin_) {
        auto mk = [&](const wchar_t* cls, const std::string& text, DWORD style, int id) {
            HWND h = CreateWindowExW(0, cls, toW(text).c_str(), WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, panel_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hInst_, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            return h;
        };
        for (const PortDesc& d : plugin_->ports()) {
            if (d.type != PortType::Control) continue;
            Row r;
            r.port = d.index;
            int id = ID_ROW_BASE + static_cast<int>(rows_.size());
            r.label = mk(L"STATIC", d.name, SS_LEFTNOWORDWRAP | SS_CENTERIMAGE | SS_ENDELLIPSIS, 0);
            if (!d.isInput) {
                r.kind = Row::Output;
                r.value = mk(L"STATIC", "", SS_LEFT | SS_CENTERIMAGE, 0);
            } else if (d.toggled) {
                r.kind = Row::Toggle;
                r.control = mk(L"BUTTON", "", BS_AUTOCHECKBOX | WS_TABSTOP, id);
                r.value = mk(L"STATIC", "", SS_LEFT | SS_CENTERIMAGE, 0);
            } else if (d.enumeration && !d.scalePoints.empty()) {
                r.kind = Row::Enum;
                r.control = mk(L"COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id);
                for (auto& sp : d.scalePoints) comboAdd(r.control, sp.label);
                r.value = mk(L"STATIC", "", SS_LEFT | SS_CENTERIMAGE, 0);
            } else {
                r.kind = Row::Slider;
                if (d.integer && (d.max - d.min) >= 1.f && (d.max - d.min) <= 1000.f) r.steps = static_cast<int>(d.max - d.min);
                r.control = mk(TRACKBAR_CLASSW, "", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, id);
                SendMessageW(r.control, TBM_SETRANGE, TRUE, MAKELPARAM(0, r.steps));
                SendMessageW(r.control, TBM_SETPAGESIZE, 0, std::max(1, r.steps / 20));
                SendMessageW(r.control, TBM_SETLINESIZE, 0, 1);
                r.value = mk(L"STATIC", "", SS_LEFT | SS_CENTERIMAGE, 0);
            }
            rows_.push_back(r);
            refreshRowDisplay(rows_.back(), plugin_->getControl(d.index));
        }
    }
    layout();
    InvalidateRect(panel_, nullptr, TRUE);
}

// ---- audio / plugin actions ----------------------------------------------

void MainWindow::startAudio() {
    stopAudio();
    int di = comboSel(cbDevice_);
    if (di < 0 || di >= static_cast<int>(devices_.size())) { Log::write("No audio device selected"); return; }
    unsigned rate = static_cast<unsigned>(std::stoul(comboText(cbRate_)));
    unsigned frames = static_cast<unsigned>(std::stoul(comboText(cbBuffer_)));
    unsigned wantIn = 2, wantOut = 2;
    if (plugin_) {
        wantIn = static_cast<unsigned>(plugin_->audioInPorts().size());
        wantOut = std::max<unsigned>(2, static_cast<unsigned>(plugin_->audioOutPorts().size()));
    }
    std::string err;
    engine_.setPlugin(nullptr);
    if (!engine_.start(devices_[di].id, devices_[di].id, rate, frames, wantIn, wantOut, err)) {
        Log::write("Audio start failed: " + err);
        MessageBoxW(hwnd_, toW(err).c_str(), L"Audio error", MB_ICONERROR);
        updateStatus();
        return;
    }
    if (plugin_ && !instantiatePlugin()) {
        // Keep the stream running as pass-through
    }
    // MIDI
    int mi = comboSel(cbMidi_);
    if (mi > 0 && mi - 1 < static_cast<int>(midiPorts_.size())) {
        std::string merr;
        if (!midi_.open(midiPorts_[mi - 1], merr)) Log::write("MIDI open failed: " + merr);
    }
    setText(btnStart_, "Stop");
    updateStatus();
}

void MainWindow::stopAudio() {
    midi_.close();
    engine_.stop();
    engine_.setPlugin(nullptr);
    if (plugin_) plugin_->deactivate();
    setText(btnStart_, "Start");
    updateStatus();
}

// Re-creates the Lv2Plugin instance for the engine's current rate / block size,
// carrying over control values. Requires the engine stream to be open.
bool MainWindow::instantiatePlugin() {
    if (!plugin_ || !engine_.isRunning()) return false;
    bool uiWasOpen = ui_ && ui_->isOpen();
    closePluginUi();
    std::vector<float> values(plugin_->ports().size());
    for (auto& p : plugin_->ports()) if (p.type == PortType::Control) values[p.index] = plugin_->getControl(p.index);
    const LilvPlugin* lp = plugin_->lilvPlugin();
    engine_.setPlugin(nullptr);
    plugin_.reset();
    plugin_ = std::make_unique<Lv2Plugin>(*world_, lp);
    std::string err;
    if (!plugin_->instantiate(engine_.actualSampleRate(), engine_.actualBufferFrames(), err)) {
        Log::write("Plugin instantiate failed: " + err);
        MessageBoxW(hwnd_, toW(err).c_str(), L"Plugin error", MB_ICONERROR);
        return false;
    }
    for (auto& p : plugin_->ports())
        if (p.type == PortType::Control && p.isInput && !p.sampleRate) plugin_->setControl(p.index, values[p.index]);
    plugin_->activate();
    engine_.setPlugin(plugin_.get());
    for (auto& r : rows_) refreshRowDisplay(r, plugin_->getControl(r.port));
    Log::write("Plugin running: " + plugin_->name());
    if (uiWasOpen) openPluginUi();
    return true;
}

void MainWindow::loadSelectedPlugin() {
    int i = comboSel(cbPlugin_);
    if (i <= 0 || i - 1 >= static_cast<int>(world_->plugins().size())) { unloadPlugin(); return; }
    loadPluginByUri(world_->plugins()[i - 1].uri);
}

void MainWindow::loadPluginByUri(const std::string& uri) {
    const LilvPlugin* lp = world_->findPlugin(uri);
    if (!lp) { Log::write("Plugin not found: " + uri); return; }
    bool wasRunning = engine_.isRunning();
    closePluginUi();
    engine_.setPlugin(nullptr);
    if (wasRunning) engine_.stop();
    plugin_.reset();
    plugin_ = std::make_unique<Lv2Plugin>(*world_, lp);
    config_.pluginUri = uri;
    Log::write("Loaded plugin: " + plugin_->name() + " (" + uri + ")");
    rebuildPanel();
    EnableWindow(btnUi_, Lv2Ui::hasWindowsUi(*world_, lp) ? TRUE : FALSE);
    setText(btnUi_, "Show UI");
    SetWindowTextW(hwnd_, (L"LoveLoveHost - " + toW(plugin_->name())).c_str());
    if (wasRunning) startAudio();
}

void MainWindow::unloadPlugin() {
    bool wasRunning = engine_.isRunning();
    closePluginUi();
    engine_.setPlugin(nullptr);
    if (wasRunning) engine_.stop();
    plugin_.reset();
    config_.pluginUri.clear();
    rebuildPanel();
    EnableWindow(btnUi_, FALSE);
    SetWindowTextW(hwnd_, L"LoveLoveHost - LV2 host");
    if (wasRunning) startAudio();
}

void MainWindow::rescanPlugins() {
    std::string current = plugin_ ? plugin_->uri() : "";
    bool wasRunning = engine_.isRunning();
    closePluginUi();
    engine_.setPlugin(nullptr);
    if (wasRunning) engine_.stop();
    plugin_.reset();
    rebuildPanel(); // destroys the old rows before their LilvPlugin goes away
    world_->rescan(config_.extraLv2Paths);
    populatePlugins();
    if (!current.empty() && world_->findPlugin(current)) loadPluginByUri(current);
    else rebuildPanel();
    if (wasRunning && !engine_.isRunning()) startAudio();
}

// ---- plugin UI window -------------------------------------------------------

void MainWindow::togglePluginUi() {
    if (ui_ && ui_->isOpen()) closePluginUi();
    else openPluginUi();
}

void MainWindow::openPluginUi() {
    if (!plugin_) return;
    if (!plugin_->isInstantiated()) {
        Log::write("Start audio before opening the plugin UI");
        MessageBoxW(hwnd_, L"Start audio first: the plugin UI needs a running instance.", L"LoveLoveHost", MB_ICONINFORMATION);
        return;
    }
    if (ui_ && ui_->isOpen()) return;
    if (!uiWnd_) {
        uiWnd_ = CreateWindowExW(0, L"LovehostPluginUi", toW(plugin_->name()).c_str(),
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, s(400), s(300), hwnd_, nullptr, hInst_, this);
    }
    ui_ = std::make_unique<Lv2Ui>(*plugin_);
    ui_->onResize = [this](int w, int h) {
        RECT r{0, 0, w, h};
        AdjustWindowRectExForDpi(&r, static_cast<DWORD>(GetWindowLongW(uiWnd_, GWL_STYLE)), FALSE, 0, GetDpiForWindow(uiWnd_));
        SetWindowPos(uiWnd_, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    };
    ui_->onControlFromUi = [this](uint32_t port, float v) {
        for (auto& r : rows_) if (r.port == port) { refreshRowDisplay(r, v); break; }
    };
    std::string err;
    if (!ui_->open(uiWnd_, err)) {
        Log::write("UI open failed: " + err);
        MessageBoxW(hwnd_, toW(err).c_str(), L"Plugin UI error", MB_ICONERROR);
        ui_.reset();
        return;
    }
    // Size the container to the widget if the UI did not call resize.
    if (HWND w = ui_->widget()) {
        RECT wr; GetWindowRect(w, &wr);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        if (ww > 10 && wh > 10) ui_->onResize(ww, wh);
        SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(uiWnd_, SW_SHOW);
    SetTimer(hwnd_, TIMER_UI, 33, nullptr);
    setText(btnUi_, "Hide UI");
    config_.showPluginUi = true;
}

void MainWindow::closePluginUi() {
    KillTimer(hwnd_, TIMER_UI);
    if (ui_) { ui_->close(); ui_.reset(); }
    if (uiWnd_) ShowWindow(uiWnd_, SW_HIDE);
    setText(btnUi_, "Show UI");
}

LRESULT MainWindow::handleUiWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        closePluginUi();
        config_.showPluginUi = false;
        return 0;
    case WM_SIZE:
        if (ui_ && ui_->widget()) SetWindowPos(ui_->widget(), nullptr, 0, 0, LOWORD(l), HIWORD(l), SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

// ---- status / log -----------------------------------------------------------

void MainWindow::updateStatus() {
    char buf[256];
    if (engine_.isRunning()) {
        snprintf(buf, sizeof(buf), "Running: %s  %u Hz  %u frames  in=%u out=%u  |  xruns %u  |  load %.0f%%  |  peak %.2f  |  %s",
                 engine_.apiName().c_str(), engine_.actualSampleRate(), engine_.actualBufferFrames(),
                 engine_.inputChannels(), engine_.outputChannels(), engine_.xruns(), engine_.cpuLoad() * 100.0,
                 engine_.peakOut(), plugin_ && plugin_->isInstantiated() ? plugin_->name().c_str() : "pass-through");
    } else {
        snprintf(buf, sizeof(buf), "Stopped  |  %s", plugin_ ? plugin_->name().c_str() : "no plugin");
    }
    setText(status_, buf);
}

void MainWindow::appendLog(const std::string& line) {
    std::wstring w = toW(line) + L"\r\n";
    int len = GetWindowTextLengthW(log_);
    if (len > 60000) SetWindowTextW(log_, L"");
    len = GetWindowTextLengthW(log_);
    SendMessageW(log_, EM_SETSEL, len, len);
    SendMessageW(log_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
}

void MainWindow::saveConfig() {
    config_.audioApi = comboText(cbApi_);
    int di = comboSel(cbDevice_);
    if (di >= 0 && di < static_cast<int>(devices_.size())) config_.outputDevice = devices_[di].name;
    std::string rate = comboText(cbRate_), frames = comboText(cbBuffer_);
    if (!rate.empty()) config_.sampleRate = static_cast<unsigned>(std::stoul(rate));
    if (!frames.empty()) config_.bufferSize = static_cast<unsigned>(std::stoul(frames));
    int mi = comboSel(cbMidi_);
    config_.midiDevice = (mi > 0 && mi - 1 < static_cast<int>(midiPorts_.size())) ? midiPorts_[mi - 1] : "";
    config_.pluginUri = plugin_ ? plugin_->uri() : "";
    config_.showPluginUi = ui_ && ui_->isOpen();
    RECT r; if (GetWindowRect(hwnd_, &r)) { config_.windowX = r.left; config_.windowY = r.top; }
    config_.save();
}

// ---- message handling -------------------------------------------------------

LRESULT MainWindow::handle(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        createWidgets();
        world_ = std::make_unique<Lv2World>(config_.extraLv2Paths);
        populateApis();
        populateDevices();
        populateBufferSizes();
        populateMidiPorts();
        populatePlugins();
        layout();
        SetTimer(hwnd_, TIMER_STATUS, 250, nullptr);
        // Plugin load / audio start happen after the window is visible (they may show dialogs).
        PostMessageW(hwnd_, WM_APP_AUTOSTART, 0, 0);
        return 0;
    }
    case WM_APP_AUTOSTART: {
        std::string want = initialPluginUri_.empty() ? config_.pluginUri : initialPluginUri_;
        if (!want.empty()) loadPluginByUri(want);
        bool autoStart = !config_.outputDevice.empty() || !initialPluginUri_.empty();
        if (autoStart && !devices_.empty()) {
            startAudio();
            if (config_.showPluginUi && plugin_ && plugin_->isInstantiated() && Lv2Ui::hasWindowsUi(*world_, plugin_->lilvPlugin()))
                openPluginUi();
        }
        if (autoExitSeconds_ > 0) SetTimer(hwnd_, TIMER_EXIT, static_cast<UINT>(autoExitSeconds_ * 1000), nullptr);
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(w);
        HFONT old = font_;
        font_ = CreateFontW(-MulDiv(9, dpi_, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (HWND c = GetWindow(hwnd_, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        for (auto& r : rows_) for (HWND c : {r.label, r.control, r.value}) if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (old) DeleteObject(old);
        const RECT* nr = reinterpret_cast<const RECT*>(l);
        SetWindowPos(hwnd_, nullptr, nr->left, nr->top, nr->right - nr->left, nr->bottom - nr->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
        mmi->ptMinTrackSize.x = s(820);
        mmi->ptMinTrackSize.y = s(360);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == ID_START && code == BN_CLICKED) { engine_.isRunning() ? stopAudio() : startAudio(); return 0; }
        if (id == ID_LOAD && code == BN_CLICKED) { loadSelectedPlugin(); return 0; }
        if (id == ID_RESCAN && code == BN_CLICKED) { rescanPlugins(); return 0; }
        if (id == ID_UI && code == BN_CLICKED) { togglePluginUi(); return 0; }
        if (id == ID_API && code == CBN_SELCHANGE) {
            bool wasRunning = engine_.isRunning();
            stopAudio();
            engine_.selectApi(comboText(cbApi_));
            config_.outputDevice.clear();
            populateDevices();
            if (wasRunning) startAudio();
            return 0;
        }
        if (id == ID_DEVICE && code == CBN_SELCHANGE) { populateSampleRates(); if (engine_.isRunning()) startAudio(); return 0; }
        if ((id == ID_RATE || id == ID_BUFFER) && code == CBN_SELCHANGE) { if (engine_.isRunning()) startAudio(); return 0; }
        if (id == ID_MIDI && code == CBN_SELCHANGE) {
            midi_.close();
            int mi = comboSel(cbMidi_);
            if (engine_.isRunning() && mi > 0 && mi - 1 < static_cast<int>(midiPorts_.size())) {
                std::string err;
                if (!midi_.open(midiPorts_[mi - 1], err)) Log::write("MIDI open failed: " + err);
            }
            return 0;
        }
        if (id == ID_PLUGIN && code == CBN_SELCHANGE) { loadSelectedPlugin(); return 0; }
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_STATUS) { updateStatus(); if (plugin_) for (auto& r : rows_) if (r.kind == Row::Output) refreshRowDisplay(r, plugin_->getControl(r.port)); }
        else if (w == TIMER_UI && ui_) { if (!ui_->idle()) { closePluginUi(); config_.showPluginUi = false; } }
        else if (w == TIMER_EXIT) {
            KillTimer(hwnd_, TIMER_EXIT);
            char buf[256];
            snprintf(buf, sizeof(buf), "Auto-exit: running=%d rate=%u frames=%u xruns=%u load=%.1f%% peak=%.3f plugin=%s",
                     engine_.isRunning() ? 1 : 0, engine_.actualSampleRate(), engine_.actualBufferFrames(), engine_.xruns(),
                     engine_.cpuLoad() * 100.0, engine_.peakOut(),
                     plugin_ && plugin_->isInstantiated() ? plugin_->name().c_str() : "none");
            Log::write(buf);
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_APP_LOG: {
        auto* sptr = reinterpret_cast<std::string*>(l);
        appendLog(*sptr);
        delete sptr;
        return 0;
    }
    case WM_CLOSE:
        saveConfig();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd_, TIMER_STATUS);
        closePluginUi();
        engine_.stop();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, m, w, l);
    }
}

LRESULT MainWindow::handlePanel(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        if (updatingControls_ || !plugin_) return 0;
        int id = LOWORD(w), code = HIWORD(w);
        int ri = id - ID_ROW_BASE;
        if (ri < 0 || ri >= static_cast<int>(rows_.size())) return 0;
        Row& r = rows_[ri];
        if (r.kind == Row::Toggle && code == BN_CLICKED) {
            bool on = SendMessageW(r.control, BM_GETCHECK, 0, 0) == BST_CHECKED;
            setRowValue(r, on ? 1.f : 0.f, false);
        } else if (r.kind == Row::Enum && code == CBN_SELCHANGE) {
            int i = comboSel(r.control);
            const PortDesc& d = plugin_->ports()[r.port];
            if (i >= 0 && i < static_cast<int>(d.scalePoints.size())) setRowValue(r, d.scalePoints[i].value, false);
        }
        return 0;
    }
    case WM_HSCROLL: {
        if (updatingControls_ || !plugin_) return 0;
        HWND ctl = reinterpret_cast<HWND>(l);
        for (auto& r : rows_) {
            if (r.kind == Row::Slider && r.control == ctl) {
                int pos = static_cast<int>(SendMessageW(ctl, TBM_GETPOS, 0, 0));
                setRowValue(r, sliderToValue(r, pos), false);
                break;
            }
        }
        return 0;
    }
    case WM_VSCROLL: {
        RECT rc; GetClientRect(panel_, &rc);
        int page = rc.bottom, maxScroll = std::max(0, panelContentHeight_ - page);
        int pos = panelScroll_;
        switch (LOWORD(w)) {
        case SB_LINEUP: pos -= s(30); break;
        case SB_LINEDOWN: pos += s(30); break;
        case SB_PAGEUP: pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: {
            SCROLLINFO si{sizeof(si), SIF_TRACKPOS};
            GetScrollInfo(panel_, SB_VERT, &si);
            pos = si.nTrackPos;
            break;
        }
        default: break;
        }
        pos = std::clamp(pos, 0, maxScroll);
        if (pos != panelScroll_) { panelScroll_ = pos; layout(); InvalidateRect(panel_, nullptr, TRUE); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        SendMessageW(panel_, WM_VSCROLL, delta > 0 ? SB_LINEUP : SB_LINEDOWN, 0);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(panel_, &rc);
        FillRect(reinterpret_cast<HDC>(w), &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    default:
        return DefWindowProcW(panel_, m, w, l);
    }
}
