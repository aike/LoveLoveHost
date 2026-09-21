#include "Lv2Ui.h"
#include "../util/Log.h"
#include <lv2/atom/atom.h>
#include <lv2/options/options.h>
#include <lv2/log/log.h>
#include <lv2/urid/urid.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
std::wstring toWide(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len > 0 ? len - 1 : 0), L' ');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

int uiLogVprintf(LV2_Log_Handle h, LV2_URID, const char* fmt, va_list ap) {
    (void)h;
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    Log::write("[ui] " + s);
    return n;
}
int uiLogPrintf(LV2_Log_Handle h, LV2_URID type, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = uiLogVprintf(h, type, fmt, ap);
    va_end(ap);
    return n;
}
}

Lv2Ui::Lv2Ui(Lv2Plugin& plugin) : plugin_(plugin) {}
Lv2Ui::~Lv2Ui() { close(); }

bool Lv2Ui::hasWindowsUi(Lv2World& world, const LilvPlugin* plugin) {
    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    if (!uis) return false;
    bool found = false;
    LILV_FOREACH(uis, i, uis) {
        if (lilv_ui_is_a(lilv_uis_get(uis, i), world.nodes.windowsUi)) { found = true; break; }
    }
    lilv_uis_free(uis);
    return found;
}

bool Lv2Ui::findUi(std::string& error) {
    Lv2World& world = plugin_.world();
    LilvUIs* uis = lilv_plugin_get_uis(plugin_.lilvPlugin());
    if (!uis) { error = "plugin has no UI"; return false; }
    bool found = false;
    LILV_FOREACH(uis, i, uis) {
        const LilvUI* ui = lilv_uis_get(uis, i);
        if (!lilv_ui_is_a(ui, world.nodes.windowsUi)) continue;
        uiUri_ = lilv_node_as_uri(lilv_ui_get_uri(ui));
        char* bin = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_binary_uri(ui)), nullptr);
        char* bun = lilv_file_uri_parse(lilv_node_as_uri(lilv_ui_get_bundle_uri(ui)), nullptr);
        if (bin) { binaryPath_ = bin; lilv_free(bin); }
        if (bun) { bundlePath_ = bun; lilv_free(bun); }
        found = true;
        break;
    }
    lilv_uis_free(uis);
    if (!found) error = "plugin has no ui:WindowsUI";
    return found;
}

bool Lv2Ui::open(HWND parent, std::string& error) {
    if (handle_) return true;
    if (!plugin_.isInstantiated()) { error = "plugin is not instantiated"; return false; }
    if (!findUi(error)) return false;

    module_ = LoadLibraryW(toWide(binaryPath_).c_str());
    if (!module_) { error = "LoadLibrary failed for " + binaryPath_ + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    auto descFn = reinterpret_cast<LV2UI_DescriptorFunction>(GetProcAddress(module_, "lv2ui_descriptor"));
    if (!descFn) { error = "lv2ui_descriptor not exported"; close(); return false; }
    descriptor_ = nullptr;
    for (uint32_t i = 0;; ++i) {
        const LV2UI_Descriptor* d = descFn(i);
        if (!d) break;
        if (uiUri_ == d->URI) { descriptor_ = d; break; }
    }
    if (!descriptor_) { error = "UI descriptor not found: " + uiUri_; close(); return false; }

    parent_ = parent;
    Lv2World& world = plugin_.world();
    const HostUrids& U = world.u();
    featMap_   = { LV2_URID__map,   world.urids().mapFeatureData() };
    featUnmap_ = { LV2_URID__unmap, world.urids().unmapFeatureData() };
    featParent_ = { LV2_UI__parent, parent };
    resize_ = { this, &Lv2Ui::resizeFn };
    featResize_ = { LV2_UI__resize, &resize_ };
    featInstance_ = { LV2_INSTANCE_ACCESS_URI, plugin_.handle() };
    dataAccess_ = { plugin_.descriptor()->extension_data };
    featData_ = { LV2_DATA_ACCESS_URI, &dataAccess_ };
    featIdle_ = { LV2_UI__idleInterface, nullptr };
    portMap_ = { this, &Lv2Ui::portIndexFn };
    featPortMap_ = { LV2_UI__portMap, &portMap_ };
    subscribe_ = { this, &Lv2Ui::subscribeFn, &Lv2Ui::unsubscribeFn };
    featSubscribe_ = { LV2_UI__portSubscribe, &subscribe_ };
    optSampleRate_ = static_cast<float>(plugin_.sampleRate());
    optScale_ = static_cast<float>(GetDpiForWindow(parent)) / 96.f;
    options_ = {
        { LV2_OPTIONS_INSTANCE, 0, U.ui_updateRate,    sizeof(float), U.atom_Float, &optUpdateRate_ },
        { LV2_OPTIONS_INSTANCE, 0, U.ui_scaleFactor,   sizeof(float), U.atom_Float, &optScale_ },
        { LV2_OPTIONS_INSTANCE, 0, U.param_sampleRate, sizeof(float), U.atom_Float, &optSampleRate_ },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr }
    };
    featOptions_ = { LV2_OPTIONS__options, options_.data() };
    log_ = { this, &uiLogPrintf, &uiLogVprintf };
    featLog_ = { LV2_LOG__log, &log_ };
    features_ = { &featMap_, &featUnmap_, &featParent_, &featResize_, &featInstance_, &featData_, &featIdle_,
                  &featPortMap_, &featSubscribe_, &featOptions_, &featLog_, nullptr };

    lastOut_.assign(plugin_.ports().size(), 0.f);
    atomBuf_.assign(65536, 0);
    widget_ = nullptr;
    LV2UI_Widget w = nullptr;
    handle_ = descriptor_->instantiate(descriptor_, plugin_.uri().c_str(), bundlePath_.c_str(),
                                       &Lv2Ui::writeFn, this, &w, features_.data());
    if (!handle_) { error = "UI instantiate failed"; close(); return false; }
    widget_ = static_cast<HWND>(w);
    if (widget_ && GetParent(widget_) != parent) SetParent(widget_, parent);
    if (widget_) ShowWindow(widget_, SW_SHOW);

    idleIface_ = descriptor_->extension_data
        ? static_cast<const LV2UI_Idle_Interface*>(descriptor_->extension_data(LV2_UI__idleInterface)) : nullptr;

    plugin_.setAtomOutForwarding(true);
    sendInitialValues();
    Log::write("Opened UI " + uiUri_);
    return true;
}

void Lv2Ui::close() {
    plugin_.setAtomOutForwarding(false);
    if (handle_ && descriptor_ && descriptor_->cleanup) descriptor_->cleanup(handle_);
    handle_ = nullptr;
    widget_ = nullptr;
    idleIface_ = nullptr;
    descriptor_ = nullptr;
    if (module_) { FreeLibrary(module_); module_ = nullptr; }
}

bool Lv2Ui::idle() {
    if (!handle_) return false;
    pumpControlOutputs();
    pumpAtomOutputs();
    if (idleIface_ && idleIface_->idle) {
        if (idleIface_->idle(handle_) != 0) return false;
    }
    return true;
}

void Lv2Ui::sendInitialValues() {
    if (!descriptor_->port_event) return;
    for (auto& p : plugin_.ports()) {
        if (p.type != PortType::Control) continue;
        float v = plugin_.getControl(p.index);
        descriptor_->port_event(handle_, p.index, sizeof(float), 0, &v);
        if (!p.isInput) lastOut_[p.index] = v;
    }
}

void Lv2Ui::notifyControl(uint32_t port, float value) {
    if (!handle_ || !descriptor_->port_event || inWrite_) return;
    descriptor_->port_event(handle_, port, sizeof(float), 0, &value);
}

void Lv2Ui::pumpControlOutputs() {
    if (!descriptor_->port_event) return;
    for (uint32_t idx : plugin_.controlOutPorts()) {
        float v = plugin_.getControl(idx);
        if (v != lastOut_[idx]) {
            lastOut_[idx] = v;
            descriptor_->port_event(handle_, idx, sizeof(float), 0, &v);
        }
    }
}

void Lv2Ui::pumpAtomOutputs() {
    if (!descriptor_->port_event) return;
    const HostUrids& U = plugin_.world().u();
    uint32_t port = 0, n = 0;
    int guard = 0;
    while ((n = plugin_.readAtomFromPlugin(port, atomBuf_.data(), static_cast<uint32_t>(atomBuf_.size()))) > 0 && guard++ < 512) {
        descriptor_->port_event(handle_, port, n, U.atom_eventTransfer, atomBuf_.data());
    }
}

// ---- callbacks from the UI ----------------------------------------------

void Lv2Ui::writeFn(LV2UI_Controller c, uint32_t port, uint32_t size, uint32_t protocol, const void* buf) {
    auto* self = static_cast<Lv2Ui*>(c);
    const HostUrids& U = self->plugin_.world().u();
    if (port >= self->plugin_.ports().size()) return;
    if (protocol == 0) {
        if (size != sizeof(float)) return;
        float v; memcpy(&v, buf, sizeof(float));
        self->plugin_.setControl(port, v);
        self->inWrite_ = true;
        if (self->onControlFromUi) self->onControlFromUi(port, v);
        self->inWrite_ = false;
    } else if (protocol == U.atom_eventTransfer) {
        if (size < sizeof(LV2_Atom)) return;
        self->plugin_.writeAtomToPlugin(port, static_cast<const LV2_Atom*>(buf));
    }
}

int Lv2Ui::resizeFn(LV2UI_Feature_Handle h, int w, int height) {
    auto* self = static_cast<Lv2Ui*>(h);
    if (self->widget_) SetWindowPos(self->widget_, nullptr, 0, 0, w, height, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    if (self->onResize) self->onResize(w, height);
    return 0;
}

uint32_t Lv2Ui::portIndexFn(LV2UI_Feature_Handle h, const char* symbol) {
    auto* self = static_cast<Lv2Ui*>(h);
    int idx = self->plugin_.portIndexBySymbol(symbol);
    return idx < 0 ? LV2UI_INVALID_PORT_INDEX : static_cast<uint32_t>(idx);
}

uint32_t Lv2Ui::subscribeFn(LV2UI_Feature_Handle, uint32_t, uint32_t, const LV2_Feature* const*) { return 0; }
uint32_t Lv2Ui::unsubscribeFn(LV2UI_Feature_Handle, uint32_t, uint32_t, const LV2_Feature* const*) { return 0; }
