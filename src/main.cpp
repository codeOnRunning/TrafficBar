#include "app.h"

#include <strsafe.h>

AppState g_app;

namespace {

BOOL CALLBACK FindWindowByClassProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0) {
        return TRUE;
    }
    if (wcscmp(className, kWindowClass) == 0) {
        *reinterpret_cast<HWND*>(lParam) = hwnd;
        return FALSE;
    }
    return TRUE;
}

HWND FindExistingWindow() {
    HWND found = nullptr;
    EnumWindows(FindWindowByClassProc, reinterpret_cast<LPARAM>(&found));
    return found;
}

enum MenuCommand : UINT {
    IDM_SHOW_CPU = 1001,
    IDM_SHOW_MEMORY = 1002,
    IDM_SHOW_DISK = 1003,
    IDM_SHOW_GPU = 1004,
    IDM_THEME_AUTO = 7001,
    IDM_THEME_DARK = 7002,
    IDM_THEME_LIGHT = 7003,
    IDM_THEME_NAVY = 7004,
    IDM_THEME_FOREST = 7005,
    IDM_ADAPTER_AUTO = 2001,
    IDM_ADAPTER_BASE = 2100,
    IDM_INTERVAL_500 = 3001,
    IDM_INTERVAL_1000 = 3002,
    IDM_INTERVAL_2000 = 3003,
    IDM_INTERVAL_5000 = 3004,
    IDM_AUTOSTART = 4001,
    IDM_ABOUT = 5001,
    IDM_HISTORY = 5002,
    IDM_PROCESS_TRAFFIC = 5003,
    IDM_EXIT = 6001,
};

std::wstring EscapeMenuText(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'&') {
            result.push_back(L'&');
        }
        result.push_back(ch);
    }
    return result;
}

std::wstring TrimTo(const std::wstring& text, size_t maxLength) {
    if (text.size() <= maxLength) {
        return text;
    }
    return text.substr(0, maxLength) + L"...";
}

void UpdateSampleTimer() {
    if (g_app.taskbarHwnd != nullptr) {
        SetTimer(g_app.taskbarHwnd, TIMER_SAMPLE, static_cast<UINT>(g_app.config.intervalMs),
                 nullptr);
    }
}

void ShowAbout(HWND owner) {
    std::wstring message = L"TrafficBar 0.15.1\n\n";
    message += L"任务栏网速 / CPU / 内存监控 + 历史流量统计\n\n";
    message += L"配置文件：\n";
    message += g_app.configPath;
    message += L"\n\n历史文件：\n";
    message += g_app.history.path();
    MessageBoxW(owner, message.c_str(), L"关于 TrafficBar", MB_OK | MB_ICONINFORMATION);
}

void HandleMenuCommand(HWND owner, UINT command, const std::vector<AdapterInfo>& adapters) {
    bool needsRefresh = true;
    bool themeChanged = false;

    if (command == IDM_SHOW_CPU) {
        g_app.config.showCpu = !g_app.config.showCpu;
    } else if (command == IDM_SHOW_MEMORY) {
        g_app.config.showMemory = !g_app.config.showMemory;
    } else if (command == IDM_SHOW_DISK) {
        g_app.config.showDisk = !g_app.config.showDisk;
    } else if (command == IDM_SHOW_GPU) {
        g_app.config.showGpu = !g_app.config.showGpu;
    } else if (command == IDM_THEME_AUTO) {
        g_app.config.themeMode = 0;
        themeChanged = true;
    } else if (command == IDM_THEME_DARK) {
        g_app.config.themeMode = 1;
        themeChanged = true;
    } else if (command == IDM_THEME_LIGHT) {
        g_app.config.themeMode = 2;
        themeChanged = true;
    } else if (command == IDM_THEME_NAVY) {
        g_app.config.themeMode = 3;
        themeChanged = true;
    } else if (command == IDM_THEME_FOREST) {
        g_app.config.themeMode = 4;
        themeChanged = true;
    } else if (command == IDM_ADAPTER_AUTO) {
        g_app.config.adapterLuid = 0;
        g_app.sampler.SetPreferredAdapter(0);
    } else if (command >= IDM_ADAPTER_BASE && command < IDM_ADAPTER_BASE + adapters.size()) {
        const size_t index = command - IDM_ADAPTER_BASE;
        if (index < adapters.size()) {
            g_app.config.adapterLuid = adapters[index].luid;
            g_app.sampler.SetPreferredAdapter(adapters[index].luid);
        }
    } else if (command == IDM_INTERVAL_500) {
        g_app.config.intervalMs = 500;
        UpdateSampleTimer();
    } else if (command == IDM_INTERVAL_1000) {
        g_app.config.intervalMs = 1000;
        UpdateSampleTimer();
    } else if (command == IDM_INTERVAL_2000) {
        g_app.config.intervalMs = 2000;
        UpdateSampleTimer();
    } else if (command == IDM_INTERVAL_5000) {
        g_app.config.intervalMs = 5000;
        UpdateSampleTimer();
    } else if (command == IDM_AUTOSTART) {
        // Migrate an existing HKCU Run entry to the scheduled task on the
        // first click, instead of accidentally disabling autostart.
        if (IsTaskAutoStartEnabled()) {
            SetAutoStart(false);
        } else {
            SetAutoStart(true);
        }
    } else if (command == IDM_ABOUT) {
        ShowAbout(owner);
        needsRefresh = false;
    } else if (command == IDM_HISTORY) {
        ShowHistoryWindow(owner);
        needsRefresh = false;
    } else if (command == IDM_PROCESS_TRAFFIC) {
        ShowProcessTrafficWindow(owner);
        needsRefresh = false;
    } else if (command == IDM_EXIT) {
        if (g_app.taskbarHwnd != nullptr) {
            PostMessageW(g_app.taskbarHwnd, WM_CLOSE, 0, 0);
        }
        return;
    } else {
        needsRefresh = false;
    }

    if (needsRefresh) {
        SaveConfig();
        if (themeChanged) {
            g_app.lastThemeSampleTick = 0;
            RefreshTaskbarWindow(true);
        } else {
            RefreshTaskbarWindow(false);
        }
    }
}

}  // namespace

void AddTrayIcon() {
    if (g_app.taskbarHwnd == nullptr) {
        return;
    }

    if (g_app.trayIcon == nullptr) {
        g_app.trayIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
        if (g_app.trayIcon == nullptr) {
            g_app.trayIcon = LoadIconW(nullptr, IDI_APPLICATION);
        }
    }

    g_app.tray = {};
    g_app.tray.cbSize = sizeof(g_app.tray);
    g_app.tray.hWnd = g_app.taskbarHwnd;
    g_app.tray.uID = kTrayIconId;
    g_app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_app.tray.uCallbackMessage = WM_TRAYICON;
    g_app.tray.hIcon = g_app.trayIcon;
    StringCchCopyW(g_app.tray.szTip, ARRAYSIZE(g_app.tray.szTip), L"TrafficBar");

    if (!Shell_NotifyIconW(NIM_ADD, &g_app.tray)) {
        Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
    }
    UpdateTrayTip();
}

void RemoveTrayIcon() {
    if (g_app.tray.hWnd != nullptr) {
        Shell_NotifyIconW(NIM_DELETE, &g_app.tray);
    }
    g_app.tray = {};
}

void UpdateTrayTip() {
    if (g_app.tray.hWnd == nullptr) {
        return;
    }

    const auto& snapshot = g_app.sampler.snapshot();
    const std::wstring up = FormatSpeed(snapshot.upBps);
    const std::wstring down = FormatSpeed(snapshot.downBps);
    const std::wstring cpu = FormatPercent(snapshot.cpuPercent);
    const std::wstring memory = FormatPercent(snapshot.memPercent);
    const DayTraffic today = g_app.history.Today();
    const std::wstring todayUp = FormatBytes(today.upBytes);
    const std::wstring todayDown = FormatBytes(today.downBytes);

    wchar_t tip[512]{};
    if (snapshot.hasNetwork) {
        swprintf_s(tip,
                   L"TrafficBar\n↑ %ls   ↓ %ls\nCPU %ls   内存 %ls\n今日 ↑ %ls  ↓ %ls\n%ls",
                   up.c_str(), down.c_str(), cpu.c_str(), memory.c_str(), todayUp.c_str(),
                   todayDown.c_str(),
                   snapshot.adapterName.empty() ? L"" : snapshot.adapterName.c_str());
    } else {
        swprintf_s(tip, L"TrafficBar\n未找到可用网络连接\n今日 ↑ %ls  ↓ %ls", todayUp.c_str(),
                   todayDown.c_str());
    }

    StringCchCopyW(g_app.tray.szTip, ARRAYSIZE(g_app.tray.szTip), tip);
    g_app.tray.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_app.tray);
}

void ShowAppMenu(HWND owner, POINT pt) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    AppendMenuW(menu, MF_STRING | (g_app.config.showCpu ? MF_CHECKED : MF_UNCHECKED), IDM_SHOW_CPU,
                L"显示 CPU");
    AppendMenuW(menu, MF_STRING | (g_app.config.showMemory ? MF_CHECKED : MF_UNCHECKED),
                IDM_SHOW_MEMORY, L"显示内存");
    AppendMenuW(menu, MF_STRING | (g_app.config.showDisk ? MF_CHECKED : MF_UNCHECKED),
                IDM_SHOW_DISK, L"显示磁盘（悬停）");
    AppendMenuW(menu, MF_STRING | (g_app.config.showGpu ? MF_CHECKED : MF_UNCHECKED),
                IDM_SHOW_GPU, L"显示 GPU（悬停）");

    HMENU themeMenu = CreatePopupMenu();
    if (themeMenu != nullptr) {
        AppendMenuW(themeMenu,
                    MF_STRING | (g_app.config.themeMode == 0 ? MF_CHECKED : MF_UNCHECKED),
                    IDM_THEME_AUTO, L"跟随任务栏");
        AppendMenuW(themeMenu,
                    MF_STRING | (g_app.config.themeMode == 1 ? MF_CHECKED : MF_UNCHECKED),
                    IDM_THEME_DARK, L"深色");
        AppendMenuW(themeMenu,
                    MF_STRING | (g_app.config.themeMode == 2 ? MF_CHECKED : MF_UNCHECKED),
                    IDM_THEME_LIGHT, L"浅色");
        AppendMenuW(themeMenu,
                    MF_STRING | (g_app.config.themeMode == 3 ? MF_CHECKED : MF_UNCHECKED),
                    IDM_THEME_NAVY, L"深蓝");
        AppendMenuW(themeMenu,
                    MF_STRING | (g_app.config.themeMode == 4 ? MF_CHECKED : MF_UNCHECKED),
                    IDM_THEME_FOREST, L"墨绿");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(themeMenu), L"外观主题");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const auto adapters = g_app.sampler.EnumerateAdapters();
    HMENU adapterMenu = CreatePopupMenu();
    if (adapterMenu != nullptr) {
        const bool autoChecked =
            g_app.config.adapterLuid == 0 ||
            g_app.sampler.activeAdapterLuid() != g_app.config.adapterLuid;
        AppendMenuW(adapterMenu, MF_STRING | (autoChecked ? MF_CHECKED : MF_UNCHECKED),
                    IDM_ADAPTER_AUTO, L"自动选择");

        size_t index = 0;
        for (const auto& adapter : adapters) {
            if (index >= 40) {
                break;
            }
            std::wstring label = adapter.name;
            if (!adapter.description.empty() && adapter.description != adapter.name) {
                label += L" - " + TrimTo(adapter.description, 38);
            }
            if (!adapter.up) {
                label += L"（未连接）";
            }
            const UINT id = IDM_ADAPTER_BASE + static_cast<UINT>(index);
            const bool checked = g_app.config.adapterLuid != 0 &&
                                 adapter.luid == g_app.config.adapterLuid;
            AppendMenuW(adapterMenu, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id,
                        EscapeMenuText(label).c_str());
            ++index;
        }
        if (index == 0) {
            AppendMenuW(adapterMenu, MF_STRING | MF_GRAYED, 0, L"没有可用网卡");
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(adapterMenu), L"网卡");
    }

    HMENU intervalMenu = CreatePopupMenu();
    if (intervalMenu != nullptr) {
        const struct {
            UINT id;
            int ms;
            const wchar_t* text;
        } intervals[] = {
            {IDM_INTERVAL_500, 500, L"0.5 秒"},
            {IDM_INTERVAL_1000, 1000, L"1 秒"},
            {IDM_INTERVAL_2000, 2000, L"2 秒"},
            {IDM_INTERVAL_5000, 5000, L"5 秒"},
        };
        for (const auto& item : intervals) {
            const UINT flags =
                MF_STRING | (g_app.config.intervalMs == item.ms ? MF_CHECKED : MF_UNCHECKED);
            AppendMenuW(intervalMenu, flags, item.id, item.text);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(intervalMenu), L"刷新间隔");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : MF_UNCHECKED), IDM_AUTOSTART,
                L"开机自启（任务计划）");
    AppendMenuW(menu, MF_STRING, IDM_HISTORY, L"流量统计...");
    AppendMenuW(menu, MF_STRING, IDM_PROCESS_TRAFFIC, L"进程流量...");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于 TrafficBar");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetForegroundWindow(owner);
    const UINT command =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x,
                       pt.y, 0, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (command != 0) {
        HandleMenuCommand(owner, command, adapters);
    }
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR commandLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&commonControls);

    const bool historyRequested = commandLine != nullptr && wcsstr(commandLine, L"--history") != nullptr;
    const bool settingsRequested =
        commandLine != nullptr && wcsstr(commandLine, L"--custom-settings") != nullptr;
    const bool processRequested =
        commandLine != nullptr && wcsstr(commandLine, L"--process-traffic") != nullptr;
    g_app.safeMode = commandLine != nullptr && wcsstr(commandLine, L"--safe-mode") != nullptr;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        const bool autoStart = commandLine != nullptr && wcsstr(commandLine, L"--autostart") != nullptr;
        if (!autoStart) {
            HWND existing = FindExistingWindow();
            if (existing != nullptr) {
                UINT message = WM_SHOWMENU;
                if (historyRequested) {
                    message = WM_SHOWHISTORY;
                } else if (settingsRequested) {
                    message = WM_SHOW_SETTINGS;
                } else if (processRequested) {
                    message = WM_SHOWPROCESS;
                }
                PostMessageW(existing, message, 0, 0);
            } else {
                MessageBoxW(nullptr, L"TrafficBar 已经在运行。", kAppName, MB_OK | MB_ICONINFORMATION);
            }
        }
        CloseHandle(mutex);
        return 0;
    }

    g_app.instance = instance;
    LoadConfig();
    g_app.history.Load(GetHistoryPath());
    g_app.lastHistorySaveTick = GetTickCount64();
    g_app.taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    AppendAppLog(g_app.safeMode ? L"TrafficBar starting in safe mode" : L"TrafficBar starting");
    if (!CreateTaskbarWindow(instance)) {
        AppendAppLog(L"TrafficBar failed to create taskbar window");
        MessageBoxW(nullptr, L"无法创建任务栏窗口。", kAppName, MB_OK | MB_ICONERROR);
        if (mutex != nullptr) {
            CloseHandle(mutex);
        }
        return 1;
    }

    EnsureAutoStartUpgrade();
    AppendAppLog(L"TrafficBar started");
    AddTrayIcon();

    if (settingsRequested) {
        ShowCustomSettings(g_app.taskbarHwnd);
    } else if (historyRequested) {
        ShowHistoryWindow(g_app.taskbarHwnd);
    } else if (processRequested) {
        ShowProcessTrafficWindow(g_app.taskbarHwnd);
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == g_app.taskbarCreatedMessage) {
            AddTrayIcon();
            g_app.lastThemeSampleTick = 0;
            RefreshTaskbarWindow(true);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_app.history.Save();
    DestroyHistoryWindow();
    RemoveTrayIcon();
    DestroyTaskbarWindow();
    if (mutex != nullptr) {
        CloseHandle(mutex);
    }
    return 0;
}
