#pragma once

#include "common.h"
#include "history.h"
#include "plugin.h"
#include "process_traffic.h"
#include "quote.h"
#include "sampler.h"

struct AppConfig {
    bool showCpu = true;
    bool showMemory = true;
    bool showDisk = false;
    bool showGpu = false;
    uint64_t adapterLuid = 0;   // 0 = auto
    int intervalMs = 1000;
    int fontSize = 9;
    int themeMode = 0;  // 0 = follow taskbar, 1 = dark, 2 = light, 3 = navy, 4 = forest

    bool customEnabled = false;
    std::wstring customPrimary;
    std::vector<CustomItem> customItems;
    int customRefreshMs = 5000;
    bool customShowPercent = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND taskbarHwnd = nullptr;
    bool safeMode = false;
    Sampler sampler;
    HistoryStore history;
    QuoteService quote;
    ProcessTrafficService processTraffic;
    PluginManager plugins;
    AppConfig config;

    HFONT font = nullptr;
    int dpi = 96;
    int width = 0;
    int height = 0;
    RECT windowRect{};
    bool taskbarVisible = false;
    bool taskbarVertical = false;
    bool lightTheme = false;
    bool sessionLocked = false;

    COLORREF backgroundColor = RGB(29, 29, 60);
    COLORREF textColor = RGB(245, 245, 245);
    COLORREF labelColor = RGB(196, 196, 212);
    COLORREF upColor = RGB(120, 215, 130);
    COLORREF downColor = RGB(120, 170, 255);

    HICON trayIcon = nullptr;
    NOTIFYICONDATAW tray{};
    UINT taskbarCreatedMessage = 0;

    std::wstring exePath;
    std::wstring configPath;

    ULONGLONG lastThemeSampleTick = 0;
    ULONGLONG lastHistorySaveTick = 0;
};

extern AppState g_app;

std::wstring GetExePath();
std::wstring GetExeDir();
std::wstring GetConfigPath();
std::wstring GetHistoryPath();
std::vector<CustomItem> ParseCustomItems(const std::wstring& text);
std::wstring FormatCustomItems(const std::vector<CustomItem>& items);
std::wstring NormalizeCustomCode(const std::wstring& code);
void LoadConfig();
void SaveConfig();
bool IsAutoStartEnabled();
bool IsTaskAutoStartEnabled();
void SetAutoStart(bool enabled);
void EnsureAutoStartUpgrade();
void AppendAppLog(const std::wstring& text);

bool CreateTaskbarWindow(HINSTANCE instance);
void DestroyTaskbarWindow();
void RefreshTaskbarWindow(bool force);
void ShowAppMenu(HWND owner, POINT pt);
void UpdateTrayTip();
void AddTrayIcon();
void RemoveTrayIcon();

std::wstring FormatSpeed(double bytesPerSecond);
std::wstring FormatPercent(double percent);

void ShowHistoryWindow(HWND owner);
void RefreshHistoryWindow();
void DestroyHistoryWindow();

void ShowProcessTrafficWindow(HWND owner);
void RefreshProcessTrafficWindow();
void DestroyProcessTrafficWindow();

void ShowCustomSettings(HWND owner);
void DestroyCustomSettings();
