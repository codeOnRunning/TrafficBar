#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <ipifcons.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "ole32.lib")

constexpr const wchar_t* kAppName = L"TrafficBar";
constexpr const wchar_t* kWindowClass = L"TrafficBar.TaskbarWindow";
constexpr const wchar_t* kMutexName = L"Local\\TrafficBar.SingleInstance";
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SHOWMENU = WM_APP + 2;
constexpr UINT WM_SHOWHISTORY = WM_APP + 3;
constexpr UINT WM_SHOWPROCESS = WM_APP + 6;
constexpr UINT WM_QUOTE_UPDATED = WM_APP + 4;
constexpr UINT WM_SHOW_SETTINGS = WM_APP + 5;
constexpr UINT WM_SHOW_APP_SETTINGS = WM_APP + 7;
constexpr int kHotkeySettings = 0xB001;
constexpr UINT_PTR TIMER_SAMPLE = 1;

struct CustomItem {
    std::wstring alias;
    std::wstring code;
};

#ifndef IF_TYPE_ETHERNET_CSMACD
#define IF_TYPE_ETHERNET_CSMACD 6
#endif
#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
#endif
#ifndef IF_TYPE_SOFTWARE_LOOPBACK
#define IF_TYPE_SOFTWARE_LOOPBACK 24
#endif
#ifndef IF_TYPE_TUNNEL
#define IF_TYPE_TUNNEL 131
#endif

inline uint64_t FileTimeToU64(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}
