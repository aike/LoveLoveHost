/*
  Minimal ui:WindowsUI for the LV2 eg-amp example plugin.
  A single trackbar controls the "gain" port (index 0). Used to verify that
  lovehost embeds native Windows plugin UIs correctly (parent, resize, idle,
  port_event and write_function).
*/
#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/urid/urid.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AMP_UI_URI "http://lovehost.example/plugins/eg-amp-ui"
#define GAIN_PORT 0
#define GAIN_MIN (-90.0f)
#define GAIN_MAX (24.0f)
#define UI_W 320
#define UI_H 90

typedef struct {
    LV2UI_Write_Function write;
    LV2UI_Controller controller;
    LV2_Log_Logger logger;
    HWND parent;
    HWND wnd;
    HWND slider;
    HWND label;
    HFONT font;
    int updating;
    int idle_calls;
} AmpUI;

static const wchar_t* kClass = L"LovehostEgAmpUI";

static void set_label(AmpUI* ui, float db) {
    wchar_t buf[64];
    swprintf(buf, 64, L"eg-amp gain: %.1f dB", db);
    SetWindowTextW(ui->label, buf);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    AmpUI* ui = (AmpUI*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
    case WM_HSCROLL:
        if (ui && !ui->updating && (HWND)l == ui->slider) {
            int pos = (int)SendMessageW(ui->slider, TBM_GETPOS, 0, 0);
            float db = GAIN_MIN + (GAIN_MAX - GAIN_MIN) * ((float)pos / 1000.0f);
            set_label(ui, db);
            ui->write(ui->controller, GAIN_PORT, sizeof(float), 0, &db);
        }
        return 0;
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, GetSysColorBrush(COLOR_WINDOW));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

static LV2UI_Handle instantiate(const LV2UI_Descriptor* descriptor, const char* plugin_uri, const char* bundle_path,
                                LV2UI_Write_Function write_function, LV2UI_Controller controller,
                                LV2UI_Widget* widget, const LV2_Feature* const* features) {
    (void)descriptor; (void)plugin_uri; (void)bundle_path;
    AmpUI* ui = (AmpUI*)calloc(1, sizeof(AmpUI));
    if (!ui) return NULL;
    ui->write = write_function;
    ui->controller = controller;
    LV2_URID_Map* map = NULL;
    LV2_Log_Log* log = NULL;
    const LV2UI_Resize* resize = NULL;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_UI__parent)) ui->parent = (HWND)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_URID__map)) map = (LV2_URID_Map*)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_LOG__log)) log = (LV2_Log_Log*)features[i]->data;
        else if (!strcmp(features[i]->URI, LV2_UI__resize)) resize = (const LV2UI_Resize*)features[i]->data;
    }
    lv2_log_logger_init(&ui->logger, map, log);
    if (!ui->parent) {
        lv2_log_error(&ui->logger, "eg-amp-ui: missing ui:parent feature\n");
        free(ui);
        return NULL;
    }
    HINSTANCE hInst = GetModuleHandleW(NULL);
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassW(&wc); /* may already exist; fine */
    ui->wnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, UI_W, UI_H, ui->parent, NULL, hInst, NULL);
    SetWindowLongPtrW(ui->wnd, GWLP_USERDATA, (LONG_PTR)ui);
    ui->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    ui->label = CreateWindowExW(0, L"STATIC", L"eg-amp gain", WS_CHILD | WS_VISIBLE, 10, 10, UI_W - 20, 22, ui->wnd, NULL, hInst, NULL);
    SendMessageW(ui->label, WM_SETFONT, (WPARAM)ui->font, TRUE);
    ui->slider = CreateWindowExW(0, TRACKBAR_CLASSW, L"", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS, 10, 40, UI_W - 20, 30, ui->wnd, NULL, hInst, NULL);
    SendMessageW(ui->slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    if (resize) resize->ui_resize(resize->handle, UI_W, UI_H);
    *widget = (LV2UI_Widget)ui->wnd;
    lv2_log_note(&ui->logger, "eg-amp-ui: instantiated\n");
    return ui;
}

static void cleanup(LV2UI_Handle handle) {
    AmpUI* ui = (AmpUI*)handle;
    lv2_log_note(&ui->logger, "eg-amp-ui: cleanup after %d idle calls\n", ui->idle_calls);
    if (ui->wnd) DestroyWindow(ui->wnd);
    if (ui->font) DeleteObject(ui->font);
    free(ui);
}

static void port_event(LV2UI_Handle handle, uint32_t port_index, uint32_t buffer_size, uint32_t format, const void* buffer) {
    AmpUI* ui = (AmpUI*)handle;
    if (port_index != GAIN_PORT || format != 0 || buffer_size != sizeof(float)) return;
    float db = *(const float*)buffer;
    float t = (db - GAIN_MIN) / (GAIN_MAX - GAIN_MIN);
    if (t < 0) t = 0; if (t > 1) t = 1;
    ui->updating = 1;
    SendMessageW(ui->slider, TBM_SETPOS, TRUE, (LPARAM)(t * 1000.0f + 0.5f));
    ui->updating = 0;
    set_label(ui, db);
    lv2_log_note(&ui->logger, "eg-amp-ui: port_event gain=%.1f\n", db);
}

static int idle(LV2UI_Handle handle) {
    AmpUI* ui = (AmpUI*)handle;
    ui->idle_calls++;
    return 0;
}

static const LV2UI_Idle_Interface idle_iface = { idle };

static const void* extension_data(const char* uri) {
    if (!strcmp(uri, LV2_UI__idleInterface)) return &idle_iface;
    return NULL;
}

static const LV2UI_Descriptor descriptor = { AMP_UI_URI, instantiate, cleanup, port_event, extension_data };

LV2_SYMBOL_EXPORT const LV2UI_Descriptor* lv2ui_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
