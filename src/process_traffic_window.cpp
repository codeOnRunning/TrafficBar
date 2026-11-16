#include "app.h"

#include <algorithm>
#include <cwchar>
#include <map>

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr int IDC_PROCESS_LIST = 2101;
constexpr int IDC_PROCESS_CLOSE = 2102;
constexpr UINT_PTR kTimerRefresh = 1;
constexpr int kHeaderHeight = 26;

constexpr const wchar_t* kProcessClass = L"TrafficBar.ProcessTraffic";

HWND g_window = nullptr;
HWND g_noteLabel = nullptr;
HWND g_processList = nullptr;
HWND g_closeButton = nullptr;
HFONT g_font = nullptr;
HBRUSH g_backgroundBrush = nullptr;
HBRUSH g_listBrush = nullptr;
COLORREF g_textColor = RGB(238, 238, 246);
COLORREF g_labelColor = RGB(180, 180, 195);
COLORREF g_listBackground = RGB(36, 36, 46);
COLORREF g_headerBackground = RGB(46, 46, 58);
COLORREF g_buttonColor = RGB(50, 50, 62);
COLORREF g_buttonBorder = RGB(86, 86, 104);
RECT g_headerRect{};
ULONGLONG g_lastRateTick = 0;
std::map<DWORD, ProcessTrafficEntry> g_previous;

struct ProcessRow {
    std::wstring name;
    DWORD pid = 0;
    double upRate = 0.0;
    double downRate = 0.0;
    uint64_t upBytes = 0;
    uint64_t downBytes = 0;
};

void EnsureBrushes() {
    const bool dark = !g_app.lightTheme;
    g_textColor = dark ? RGB(238, 238, 246) : RGB(30, 30, 30);
    g_labelColor = dark ? RGB(180, 180, 195) : RGB(80, 80, 80);
    g_listBackground = dark ? RGB(36, 36, 46) : RGB(255, 255, 255);
    g_headerBackground = dark ? RGB(46, 46, 58) : RGB(240, 240, 240);
    g_buttonColor = dark ? RGB(50, 50, 62) : RGB(238, 238, 238);
    g_buttonBorder = dark ? RGB(86, 86, 104) : RGB(180, 180, 180);

    if (g_backgroundBrush != nullptr) {
        DeleteObject(g_backgroundBrush);
    }
    if (g_listBrush != nullptr) {
        DeleteObject(g_listBrush);
    }
    g_backgroundBrush = CreateSolidBrush(dark ? RGB(30, 30, 38) : RGB(246, 246, 246));
    g_listBrush = CreateSolidBrush(g_listBackground);
}

void ApplyTheme(HWND hwnd) {
    EnsureBrushes();
    const BOOL darkTitle = g_app.lightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkTitle, sizeof(darkTitle));

    const wchar_t* theme = g_app.lightTheme ? L"Explorer" : L"DarkMode_Explorer";
    SetWindowTheme(g_processList, theme, nullptr);
    SetWindowTheme(g_processList, theme, L"ScrollBar");
    ListView_SetBkColor(g_processList, g_listBackground);
    ListView_SetTextBkColor(g_processList, g_listBackground);
    ListView_SetTextColor(g_processList, g_textColor);
    ListView_SetExtendedListViewStyle(
        g_processList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                           (g_app.lightTheme ? LVS_EX_GRIDLINES : 0));
    InvalidateRect(hwnd, nullptr, TRUE);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void DrawButton(const DRAWITEMSTRUCT* item) {
    if (item == nullptr) {
        return;
    }
    RECT rect = item->rcItem;
    COLORREF background = g_buttonColor;
    if ((item->itemState & ODS_SELECTED) != 0) {
        background = g_app.lightTheme ? RGB(214, 214, 214) : RGB(36, 36, 46);
    }
    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, g_buttonBorder);
    HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
    HGDIOBJ oldPen = SelectObject(item->hDC, pen);
    RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, 6, 6);
    SelectObject(item->hDC, oldPen);
    SelectObject(item->hDC, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);

    wchar_t text[64]{};
    GetWindowTextW(item->hwndItem, text, ARRAYSIZE(text));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, g_textColor);
    HGDIOBJ oldFont = SelectObject(item->hDC, g_font);
    DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, oldFont);
}

void DrawHeader(HDC dc) {
    if (g_processList == nullptr || g_headerRect.right <= g_headerRect.left) {
        return;
    }
    HBRUSH brush = CreateSolidBrush(g_headerBackground);
    FillRect(dc, &g_headerRect, brush);
    DeleteObject(brush);

    const wchar_t* titles[] = {L"进程", L"PID", L"上传/s", L"下载/s", L"上传总量", L"下载总量"};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_textColor);
    HGDIOBJ oldFont = SelectObject(dc, g_font);
    int x = g_headerRect.left;
    for (int i = 0; i < 6; ++i) {
        const int columnWidth = ListView_GetColumnWidth(g_processList, i);
        RECT textRect{x + 4, g_headerRect.top, x + columnWidth - 2, g_headerRect.bottom};
        DrawTextW(dc, titles[i], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += columnWidth;
    }
    SelectObject(dc, oldFont);

    HPEN pen = CreatePen(PS_SOLID, 1, g_buttonBorder);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, g_headerRect.left, g_headerRect.bottom - 1, nullptr);
    LineTo(dc, g_headerRect.right, g_headerRect.bottom - 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void Layout(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int margin = 16;
    const int headerTop = 42;
    const int buttonHeight = 30;

    if (g_noteLabel != nullptr) {
        MoveWindow(g_noteLabel, margin, 12, width - margin * 2, 22, TRUE);
    }
    g_headerRect = {margin, headerTop, width - margin, headerTop + kHeaderHeight};
    if (g_processList != nullptr) {
        MoveWindow(g_processList, margin, headerTop + kHeaderHeight, width - margin * 2,
                   std::max(70, static_cast<int>(client.bottom) - headerTop - kHeaderHeight - 58),
                   TRUE);
    }
    if (g_closeButton != nullptr) {
        MoveWindow(g_closeButton, width - margin - 84, client.bottom - margin - buttonHeight, 84,
                   buttonHeight, TRUE);
    }
}

std::vector<ProcessRow> BuildRows(const std::vector<ProcessTrafficEntry>& snapshot,
                                  ULONGLONG now) {
    const double elapsed = g_lastRateTick == 0
                               ? 0.0
                               : std::max(0.001, static_cast<double>(now - g_lastRateTick) / 1000.0);
    std::vector<ProcessRow> rows;
    rows.reserve(snapshot.size());
    for (const auto& entry : snapshot) {
        ProcessRow row;
        row.name = entry.name;
        row.pid = entry.pid;
        row.upBytes = entry.upBytes;
        row.downBytes = entry.downBytes;
        const auto previous = g_previous.find(entry.pid);
        if (previous != g_previous.end() && elapsed > 0.0) {
            const uint64_t upDelta =
                entry.upBytes >= previous->second.upBytes ? entry.upBytes - previous->second.upBytes : 0;
            const uint64_t downDelta = entry.downBytes >= previous->second.downBytes
                                           ? entry.downBytes - previous->second.downBytes
                                           : 0;
            row.upRate = static_cast<double>(upDelta) / elapsed;
            row.downRate = static_cast<double>(downDelta) / elapsed;
        }
        rows.push_back(std::move(row));
    }

    g_previous.clear();
    for (const auto& entry : snapshot) {
        g_previous[entry.pid] = entry;
    }
    g_lastRateTick = now;

    std::sort(rows.begin(), rows.end(), [](const ProcessRow& a, const ProcessRow& b) {
        const double aRate = a.upRate + a.downRate;
        const double bRate = b.upRate + b.downRate;
        if (aRate != bRate) {
            return aRate > bRate;
        }
        return a.upBytes + a.downBytes > b.upBytes + b.downBytes;
    });
    if (rows.size() > 100) {
        rows.resize(100);
    }
    return rows;
}

void RefreshList() {
    if (g_processList == nullptr) {
        return;
    }
    const auto snapshot = g_app.processTraffic.Snapshot();
    const auto rows = BuildRows(snapshot, GetTickCount64());
    ListView_DeleteAllItems(g_processList);

    int index = 0;
    for (const auto& row : rows) {
        const std::wstring pid = std::to_wstring(row.pid);
        const std::wstring upRate = FormatSpeed(row.upRate);
        const std::wstring downRate = FormatSpeed(row.downRate);
        const std::wstring upTotal = FormatBytes(row.upBytes);
        const std::wstring downTotal = FormatBytes(row.downBytes);

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = const_cast<wchar_t*>(row.name.c_str());
        const int inserted = ListView_InsertItem(g_processList, &item);
        if (inserted < 0) {
            break;
        }
        ListView_SetItemText(g_processList, inserted, 1, const_cast<wchar_t*>(pid.c_str()));
        ListView_SetItemText(g_processList, inserted, 2, const_cast<wchar_t*>(upRate.c_str()));
        ListView_SetItemText(g_processList, inserted, 3, const_cast<wchar_t*>(downRate.c_str()));
        ListView_SetItemText(g_processList, inserted, 4, const_cast<wchar_t*>(upTotal.c_str()));
        ListView_SetItemText(g_processList, inserted, 5, const_cast<wchar_t*>(downTotal.c_str()));
        ++index;
    }

    if (index == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<wchar_t*>(L"暂无进程流量数据");
        ListView_InsertItem(g_processList, &item);
    }
}

void CreateControls(HWND hwnd) {
    const HINSTANCE instance = g_app.instance;
    g_font = CreateFontW(-MulDiv(10, g_app.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    g_noteLabel = CreateWindowExW(0, L"STATIC", L"按进程统计 TCP/UDP 流量（ETW）",
                                  WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, instance,
                                  nullptr);
    g_processList = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_NOCOLUMNHEADER |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROCESS_LIST)), instance,
        nullptr);
    ListView_SetExtendedListViewStyle(
        g_processList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.fmt = LVCFMT_LEFT;
    column.cx = 150;
    column.pszText = const_cast<wchar_t*>(L"进程");
    ListView_InsertColumn(g_processList, 0, &column);
    column.cx = 62;
    column.pszText = const_cast<wchar_t*>(L"PID");
    ListView_InsertColumn(g_processList, 1, &column);
    column.cx = 92;
    column.pszText = const_cast<wchar_t*>(L"上传/s");
    ListView_InsertColumn(g_processList, 2, &column);
    column.cx = 92;
    column.pszText = const_cast<wchar_t*>(L"下载/s");
    ListView_InsertColumn(g_processList, 3, &column);
    column.cx = 100;
    column.pszText = const_cast<wchar_t*>(L"上传总量");
    ListView_InsertColumn(g_processList, 4, &column);
    column.cx = 100;
    column.pszText = const_cast<wchar_t*>(L"下载总量");
    ListView_InsertColumn(g_processList, 5, &column);

    g_closeButton = CreateWindowExW(0, L"BUTTON", L"关闭",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                    hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PROCESS_CLOSE)),
                                    instance, nullptr);

    if (g_font != nullptr) {
        for (HWND control : {g_noteLabel, g_processList, g_closeButton}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
        }
    }

    Layout(hwnd);
    ApplyTheme(hwnd);
    RefreshList();
    SetTimer(hwnd, kTimerRefresh, 1000, nullptr);
}

LRESULT CALLBACK ProcessWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateControls(hwnd);
        return 0;
    case WM_SIZE:
        Layout(hwnd);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 600;
        info->ptMinTrackSize.y = 360;
        return 0;
    }
    case WM_ERASEBKGND: {
        if (g_backgroundBrush == nullptr) {
            EnsureBrushes();
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, g_backgroundBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_backgroundBrush);
        DrawHeader(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_labelColor);
        return reinterpret_cast<LRESULT>(g_backgroundBrush);
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_backgroundBrush);
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_TIMER:
        if (wParam == kTimerRefresh) {
            RefreshList();
            return 0;
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROCESS_CLOSE) {
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        g_app.lastThemeSampleTick = 0;
        RefreshTaskbarWindow(true);
        ApplyTheme(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTimerRefresh);
        g_window = nullptr;
        g_noteLabel = nullptr;
        g_processList = nullptr;
        g_closeButton = nullptr;
        if (g_font != nullptr) {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        if (g_backgroundBrush != nullptr) {
            DeleteObject(g_backgroundBrush);
            g_backgroundBrush = nullptr;
        }
        if (g_listBrush != nullptr) {
            DeleteObject(g_listBrush);
            g_listBrush = nullptr;
        }
        g_previous.clear();
        g_lastRateTick = 0;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void ShowProcessTrafficWindow(HWND owner) {
    (void)owner;
    if (!g_app.safeMode && !g_app.processTraffic.running()) {
        g_app.processTraffic.Start();
    }
    if (g_window != nullptr && IsWindow(g_window)) {
        RefreshList();
        ShowWindow(g_window, IsIconic(g_window) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_window);
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ProcessWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.lpszClassName = kProcessClass;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    g_window = CreateWindowExW(
        0, kProcessClass, L"进程流量 - TrafficBar",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 640,
        480, nullptr, nullptr, g_app.instance, nullptr);
    if (g_window == nullptr) {
        return;
    }

    RECT workArea{};
    RECT windowRect{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        GetWindowRect(g_window, &windowRect)) {
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        SetWindowPos(g_window, nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(g_window, IsIconic(g_window) ? SW_RESTORE : SW_SHOW);
    UpdateWindow(g_window);
    SetForegroundWindow(g_window);
}

void RefreshProcessTrafficWindow() {
    if (g_window != nullptr && IsWindow(g_window)) {
        RefreshList();
    }
}

void DestroyProcessTrafficWindow() {
    if (g_window != nullptr && IsWindow(g_window)) {
        DestroyWindow(g_window);
    }
    g_window = nullptr;
}
