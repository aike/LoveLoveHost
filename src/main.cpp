// LoveLoveHost - standalone single-plugin LV2 host for Windows (ASIO via RtAudio)
#include "gui/MainWindow.h"
#include "util/Config.h"
#include "util/Log.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <filesystem>
#include <string>
#include <windows.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
std::string toU8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(len > 0 ? len - 1 : 0), ' ');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Optional arguments:
    //   LoveLoveHost.exe [plugin-uri] [--lv2-path <dir>]... [--api <name>] [--device <name>] [--show-ui] [--run-seconds <n>]
    std::string pluginUri;
    int runSeconds = 0;
    Config config;
    config.load();
    std::string logPath = Config::defaultPath();
    logPath = logPath.substr(0, logPath.find_last_of("\\/")) + "\\LoveLoveHost.log";
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(logPath).parent_path(), ec);
    }
    Log::openFile(logPath);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::string a = toU8(argv[i]);
        if (a == "--lv2-path" && i + 1 < argc) config.extraLv2Paths.push_back(toU8(argv[++i]));
        else if (a == "--api" && i + 1 < argc) config.audioApi = toU8(argv[++i]);
        else if (a == "--device" && i + 1 < argc) config.outputDevice = toU8(argv[++i]);
        else if (a == "--run-seconds" && i + 1 < argc) runSeconds = std::stoi(toU8(argv[++i]));
        else if (a == "--show-ui") config.showPluginUi = true;
        else if (!a.empty() && a[0] != '-') pluginUri = a;
    }
    if (argv) LocalFree(argv);

    MainWindow win(hInst, config, pluginUri, runSeconds);
    if (!win.create()) {
        MessageBoxW(nullptr, L"Failed to create the main window.", L"LoveLoveHost", MB_ICONERROR);
        return 1;
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(win.hwnd(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
