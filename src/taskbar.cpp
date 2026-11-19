#include "app.h"

#include <cwchar>
#include <wtsapi32.h>

namespace {

constexpr int kPadding = 6;
constexpr int kArrowWidth = 11;
constexpr int kColumnGap = 6;
constexpr int kMinWidth = 90;
constexpr UINT_PTR kTimerZOrder = 2;

constexpr const wchar_t* kHoverTipClass = L"TrafficBar.HoverTip";
constexpr const wchar_t* kSecondaryTaskbarClass = L"TrafficBar.SecondaryTaskbar";
constexpr UINT_PTR kTimerHoverShow = 3;
constexpr UINT_PTR kTimerThemeRefresh = 4;
constexpr int kHoverVisibleStocks = 3;
constexpr int kHoverScrollbarWidth = 8;

struct SecondaryTaskbarSurface {
    HWND window = nullptr;
    HWND taskbar = nullptr;
    bool vertical = false;
};

std::vector<SecondaryTaskbarSurface> g_secondaryTaskbars;

HWND g_hoverTip = nullptr;
HWND g_hoverTaskbar = nullptr;
RECT g_hoverSurfaceRect{};
HFONT g_hoverFont = nullptr;
bool g_hoverVisible = false;
std::vector<std::wstring> g_hoverLines;
std::vector<std::wstring> g_hoverStockLines;
int g_hoverScrollOffset = 0;
bool g_hoverHasScrollbar = false;
int g_hoverWidth = 0;
int g_hoverHeight = 0;
RECT g_hoverThumbRect{};
int g_hoverTrackTop = 0;
int g_hoverTrackHeight = 0;
bool g_hoverDragging = false;
int g_hoverDragStartY = 0;
int g_hoverDragStartOffset = 0;

void PaintTaskbar(HWND hwnd);
void ShowHoverTip();
void RefreshSecondaryTaskbars(bool force);
void DestroySecondaryTaskbars();

void ConfigureQuoteService() {
    if (g_app.config.customEnabled && !g_app.config.customItems.empty()) {
        g_app.quote.Configure(g_app.config.customItems, g_app.config.customRefreshMs);
    } else {
        const std::vector<CustomItem> empty;
        g_app.quote.Configure(empty, g_app.config.customRefreshMs);
    }
}

void EnsureHoverFont() {
    if (g_hoverFont != nullptr) {
        return;
    }
    const int height = -MulDiv(9, g_app.dpi, 72);
    g_hoverFont = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (g_hoverFont == nullptr) {
        g_hoverFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

std::wstring FormatQuotePrice(double price) {
    wchar_t buffer[32]{};
    if (price >= 1.0) {
        swprintf_s(buffer, L"%.2f", price);
    } else {
        swprintf_s(buffer, L"%.3f", price);
    }
    return buffer;
}

std::wstring FormatQuoteChange(double percent) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, g_app.config.customShowPercent ? L"%+.2f%%" : L"%+.2f", percent);
    return buffer;
}

std::wstring FormatQuoteText(const QuoteSnapshot& quote) {
    if (!quote.valid) {
        return quote.alias + L" --";
    }
    return quote.alias + L" " + FormatQuotePrice(quote.price) + L" " +
           FormatQuoteChange(quote.changePercent);
}

const QuoteSnapshot* FindPrimaryQuote(const std::vector<QuoteSnapshot>& quotes) {
    if (quotes.empty()) {
        return nullptr;
    }
    if (!g_app.config.customPrimary.empty()) {
        for (const auto& quote : quotes) {
            if (_wcsicmp(quote.alias.c_str(), g_app.config.customPrimary.c_str()) == 0) {
                return &quote;
            }
        }
    }
    return &quotes.front();
}

std::wstring PrimaryQuoteText() {
    if (!g_app.config.customEnabled) {
        return L"";
    }
    const std::vector<QuoteSnapshot> quotes = g_app.quote.Snapshot();
    const QuoteSnapshot* quote = FindPrimaryQuote(quotes);
    if (quote == nullptr) {
        return g_app.config.customPrimary + L" --";
    }
    return FormatQuoteText(*quote);
}

void BuildHoverLines() {
    g_hoverLines.clear();
    g_hoverStockLines.clear();
    const auto& snapshot = g_app.sampler.snapshot();
    g_hoverLines.push_back(L"↑ " + FormatSpeed(snapshot.upBps) + L"   ↓ " +
                           FormatSpeed(snapshot.downBps));
    std::wstring cpuLine = L"CPU " + FormatPercent(snapshot.cpuPercent);
    if (snapshot.cpuTempAvailable) {
        cpuLine += L"  " + std::to_wstring(snapshot.cpuTempC) + L"°C";
    }
    g_hoverLines.push_back(std::move(cpuLine));
    if (snapshot.memTotalBytes > 0) {
        g_hoverLines.push_back(L"内存 " + FormatBytes(snapshot.memUsedBytes) + L" / " +
                               FormatBytes(snapshot.memTotalBytes) + L" (" +
                               FormatPercent(snapshot.memPercent) + L")");
    } else {
        g_hoverLines.push_back(L"内存读取中...");
    }
    if (g_app.config.showDisk && snapshot.diskTotalBytes > 0) {
        std::wstring diskLine = L"磁盘 " + snapshot.diskName + L" " +
                                FormatPercent(snapshot.diskPercent);
        if (snapshot.diskSmartAvailable) {
            diskLine += snapshot.diskSmartHealthy ? L" 正常" : L" 警告";
            if (snapshot.diskSmartTempC > 0) {
                diskLine += L" " + std::to_wstring(snapshot.diskSmartTempC) + L"°C";
            }
        }
        g_hoverLines.push_back(std::move(diskLine));
    }
    if (g_app.config.showGpu) {
        std::wstring gpuLine =
            snapshot.gpuAvailable ? L"GPU " + FormatPercent(snapshot.gpuPercent) : L"GPU --";
        if (snapshot.gpuTempAvailable) {
            gpuLine += L"  " + std::to_wstring(snapshot.gpuTempC) + L"°C";
        }
        g_hoverLines.push_back(std::move(gpuLine));
    }
    for (const auto& line : g_app.plugins.Lines()) {
        if (!line.empty()) {
            g_hoverLines.push_back(line);
        }
    }

    if (g_app.config.customEnabled) {
        const std::vector<QuoteSnapshot> quotes = g_app.quote.Snapshot();
        for (const auto& quote : quotes) {
            g_hoverStockLines.push_back(FormatQuoteText(quote));
        }
    }

    g_hoverHasScrollbar = g_hoverStockLines.size() > kHoverVisibleStocks;
    const int maxOffset = std::max(0, static_cast<int>(g_hoverStockLines.size()) -
                                          kHoverVisibleStocks);
    g_hoverScrollOffset = std::clamp(g_hoverScrollOffset, 0, maxOffset);
}

void MeasureHoverTip() {
    HDC dc = GetDC(g_hoverTip);
    if (dc == nullptr) {
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, g_hoverFont);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + 2;
    int maxWidth = 0;
    for (const auto& line : g_hoverLines) {
        SIZE size{};
        GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &size);
        maxWidth = std::max(maxWidth, static_cast<int>(size.cx));
    }
    for (const auto& line : g_hoverStockLines) {
        SIZE size{};
        GetTextExtentPoint32W(dc, line.c_str(), static_cast<int>(line.size()), &size);
        maxWidth = std::max(maxWidth, static_cast<int>(size.cx));
    }
    const int visibleStocks = std::min(static_cast<int>(g_hoverStockLines.size()),
                                       kHoverVisibleStocks);
    SelectObject(dc, oldFont);
    ReleaseDC(g_hoverTip, dc);

    g_hoverWidth = maxWidth + 18;
    if (g_hoverHasScrollbar) {
        g_hoverWidth += kHoverScrollbarWidth;
    }
    g_hoverHeight =
        lineHeight * (static_cast<int>(g_hoverLines.size()) + visibleStocks) + 12;
}

void UpdateHoverScrollInfo() {
    const int maxOffset = std::max(0, static_cast<int>(g_hoverStockLines.size()) -
                                          kHoverVisibleStocks);
    g_hoverScrollOffset = std::clamp(g_hoverScrollOffset, 0, maxOffset);
}

void ScrollHoverTo(int position) {
    const int maxOffset = std::max(0, static_cast<int>(g_hoverStockLines.size()) -
                                          kHoverVisibleStocks);
    const int clamped = std::clamp(position, 0, maxOffset);
    if (clamped == g_hoverScrollOffset) {
        return;
    }
    g_hoverScrollOffset = clamped;
    UpdateHoverScrollInfo();
    InvalidateRect(g_hoverTip, nullptr, FALSE);
}

void PaintHoverTip(HWND hwnd) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH background = CreateSolidBrush(g_app.backgroundColor);
    FillRect(dc, &client, background);
    DeleteObject(background);

    const COLORREF borderColor =
        g_app.lightTheme ? RGB(180, 180, 180) : RGB(82, 82, 124);
    HBRUSH border = CreateSolidBrush(borderColor);
    FrameRect(dc, &client, border);
    DeleteObject(border);

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, g_hoverFont);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    const int lineHeight = metrics.tmHeight + 2;
    int y = 6;
    for (const auto& line : g_hoverLines) {
        SetTextColor(dc, g_app.textColor);
        TextOutW(dc, 9, y, line.c_str(), static_cast<int>(line.size()));
        y += lineHeight;
    }
    const int visibleStocks = std::min(static_cast<int>(g_hoverStockLines.size()),
                                       kHoverVisibleStocks);
    for (int i = 0; i < visibleStocks; ++i) {
        const std::wstring& line = g_hoverStockLines[static_cast<size_t>(g_hoverScrollOffset + i)];
        SetTextColor(dc, g_app.textColor);
        TextOutW(dc, 9, y, line.c_str(), static_cast<int>(line.size()));
        y += lineHeight;
    }

    if (g_hoverHasScrollbar && visibleStocks > 0) {
        const int stockTop = 6 + static_cast<int>(g_hoverLines.size()) * lineHeight;
        const int stockHeight = visibleStocks * lineHeight;
        const int trackTop = stockTop + 2;
        const int trackBottom = stockTop + stockHeight - 2;
        g_hoverTrackTop = trackTop;
        g_hoverTrackHeight = std::max(1, trackBottom - trackTop);

        const int total = static_cast<int>(g_hoverStockLines.size());
        const int thumbHeight =
            std::max(12, g_hoverTrackHeight * kHoverVisibleStocks / std::max(1, total));
        const int maxOffset = std::max(0, total - kHoverVisibleStocks);
        const int thumbY =
            trackTop + (g_hoverTrackHeight - thumbHeight) * g_hoverScrollOffset /
                            std::max(1, maxOffset);
        const int thumbX = client.right - 5;
        g_hoverThumbRect = {thumbX, thumbY, thumbX + 3, thumbY + thumbHeight};

        HBRUSH thumbBrush =
            CreateSolidBrush(g_app.lightTheme ? RGB(160, 160, 160) : RGB(84, 84, 108));
        HGDIOBJ oldBrush = SelectObject(dc, thumbBrush);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        RoundRect(dc, g_hoverThumbRect.left, g_hoverThumbRect.top, g_hoverThumbRect.right,
                  g_hoverThumbRect.bottom, 3, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(thumbBrush);
    } else {
        SetRectEmpty(&g_hoverThumbRect);
        g_hoverTrackTop = 0;
        g_hoverTrackHeight = 0;
    }
    SelectObject(dc, oldFont);
    EndPaint(hwnd, &paint);
}

void PositionHoverTip() {
    if (g_hoverTip == nullptr) {
        return;
    }

    BuildHoverLines();
    MeasureHoverTip();
    UpdateHoverScrollInfo();

    HWND hoverTaskbar = g_hoverTaskbar != nullptr ? g_hoverTaskbar : g_app.taskbarHwnd;
    RECT taskbar{};
    if (hoverTaskbar == nullptr || !GetWindowRect(hoverTaskbar, &taskbar)) {
        return;
    }
    const bool vertical = (taskbar.bottom - taskbar.top) > (taskbar.right - taskbar.left);
    int x = 0;
    int y = 0;
    if (vertical) {
        x = taskbar.right + 4;
        y = g_hoverSurfaceRect.bottom > g_hoverSurfaceRect.top ? g_hoverSurfaceRect.top
                                                                : g_app.windowRect.top;
    } else {
        x = taskbar.right - g_hoverWidth;
        y = taskbar.top - g_hoverHeight - 4;
    }

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(g_app.taskbarHwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &monitorInfo)) {
        if (vertical) {
            if (x + g_hoverWidth > monitorInfo.rcWork.right) {
                x = taskbar.left - g_hoverWidth - 4;
            }
            if (y + g_hoverHeight > monitorInfo.rcWork.bottom) {
                y = monitorInfo.rcWork.bottom - g_hoverHeight;
            }
        } else if (y < monitorInfo.rcWork.top) {
            y = taskbar.bottom + 4;
        }
        if (x < monitorInfo.rcWork.left) {
            x = monitorInfo.rcWork.left;
        }
        if (y < monitorInfo.rcWork.top) {
            y = monitorInfo.rcWork.top;
        }
    }

    SetWindowPos(g_hoverTip, HWND_TOPMOST, x, y, g_hoverWidth, g_hoverHeight,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(g_hoverTip, SW_SHOWNOACTIVATE);
}

void ShowHoverTipForSurface(HWND taskbar, HWND surface) {
    g_hoverTaskbar = taskbar;
    if (surface != nullptr) {
        GetWindowRect(surface, &g_hoverSurfaceRect);
    }
    ShowHoverTip();
}

void ShowHoverTip() {
    if (g_hoverTip == nullptr) {
        return;
    }
    PositionHoverTip();
    g_hoverVisible = true;
    InvalidateRect(g_hoverTip, nullptr, FALSE);
}

void UpdateHoverTip() {
    if (!g_hoverVisible) {
        return;
    }
    PositionHoverTip();
    InvalidateRect(g_hoverTip, nullptr, FALSE);
}

void HideHoverTip() {
    if (g_hoverTip != nullptr && g_hoverVisible) {
        ShowWindow(g_hoverTip, SW_HIDE);
    }
    g_hoverVisible = false;
    g_hoverScrollOffset = 0;
    g_hoverTaskbar = nullptr;
    g_hoverSurfaceRect = {};
}

LRESULT CALLBACK HoverTipWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        PaintHoverTip(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEWHEEL:
        if (g_hoverHasScrollbar) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            ScrollHoverTo(g_hoverScrollOffset - delta);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!g_hoverHasScrollbar) {
            return 0;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT hit = g_hoverThumbRect;
        InflateRect(&hit, 5, 3);
        if (PtInRect(&hit, point)) {
            g_hoverDragging = true;
            g_hoverDragStartY = point.y;
            g_hoverDragStartOffset = g_hoverScrollOffset;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_hoverDragging) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int thumbHeight = g_hoverThumbRect.bottom - g_hoverThumbRect.top;
            const int trackRange = std::max(1, g_hoverTrackHeight - thumbHeight);
            const int maxOffset = std::max(0, static_cast<int>(g_hoverStockLines.size()) -
                                                  kHoverVisibleStocks);
            const int offset =
                g_hoverDragStartOffset +
                (point.y - g_hoverDragStartY) * maxOffset / trackRange;
            ScrollHoverTo(offset);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_hoverDragging) {
            g_hoverDragging = false;
            ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_hoverDragging = false;
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

bool CreateHoverTipWindow() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = HoverTipWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kHoverTipClass;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    EnsureHoverFont();
    g_hoverTip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kHoverTipClass,
                                 L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                                 g_app.instance, nullptr);
    return g_hoverTip != nullptr;
}

void DestroyHoverTip() {
    if (g_hoverTip != nullptr) {
        DestroyWindow(g_hoverTip);
        g_hoverTip = nullptr;
    }
    if (g_hoverFont != nullptr) {
        DeleteObject(g_hoverFont);
        g_hoverFont = nullptr;
    }
    g_hoverVisible = false;
}

void HideHoverTipIfLeft() {
    if (!g_hoverVisible || g_hoverTip == nullptr || g_app.taskbarHwnd == nullptr) {
        return;
    }
    if (g_hoverDragging) {
        return;
    }
    POINT cursor{};
    RECT taskbarRect{};
    HWND hoverTaskbar = g_hoverTaskbar != nullptr ? g_hoverTaskbar : g_app.taskbarHwnd;
    if (!GetCursorPos(&cursor) || hoverTaskbar == nullptr ||
        !GetWindowRect(hoverTaskbar, &taskbarRect)) {
        return;
    }
    if (PtInRect(&taskbarRect, cursor)) {
        return;
    }
    RECT hoverRect{};
    if (GetWindowRect(g_hoverTip, &hoverRect)) {
        InflateRect(&hoverRect, 8, 8);
        if (PtInRect(&hoverRect, cursor)) {
            return;
        }
    }
    HideHoverTip();
}

void EnsureAboveTaskbar() {
    if (g_app.taskbarHwnd == nullptr || !IsWindowVisible(g_app.taskbarHwnd)) {
        return;
    }

    // Do not keep reasserting HWND_TOPMOST in response to taskbar clicks.
    // Reordering a topmost popup against the taskbar on every click makes the
    // compact widget visibly blink. The window is already created topmost;
    // only repair it if that extended style was actually lost.
    if ((GetWindowLongPtrW(g_app.taskbarHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0) {
        return;
    }

    SetWindowPos(g_app.taskbarHwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER |
                     SWP_NOREDRAW);
}

int MeasureText(HDC dc, const std::wstring& text) {
    SIZE size{};
    if (text.empty()) {
        return 0;
    }
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    return size.cx;
}

void EnsureFont(int dpi, bool force = false) {
    if (!force && g_app.font != nullptr && g_app.dpi == dpi) {
        return;
    }
    if (g_app.font != nullptr) {
        DeleteObject(g_app.font);
        g_app.font = nullptr;
    }
    g_app.dpi = dpi;
    const int height = -MulDiv(g_app.config.fontSize, dpi, 72);
    g_app.font = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (g_app.font == nullptr) {
        g_app.font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

int SpeedColumnWidth(HDC dc) {
    const int a = MeasureText(dc, L"999.9 MB/s");
    const int b = MeasureText(dc, L"1.00 GB/s");
    return std::max(a, b) + kArrowWidth + 3;
}

int CustomColumnWidth(HDC dc) {
    if (!g_app.config.customEnabled || g_app.config.customItems.empty()) {
        return 0;
    }
    std::wstring sample = g_app.config.customPrimary;
    if (sample.empty()) {
        sample = g_app.config.customItems.front().alias;
    }
    sample += L" 0000.00 +0.00";
    if (g_app.config.customShowPercent) {
        sample += L"%";
    }
    return MeasureText(dc, sample);
}

int InfoColumnWidth(HDC dc) {
    int width = 0;
    if (g_app.config.showCpu) {
        width = std::max(width, MeasureText(dc, L"CPU 100%"));
    }

    int rowOneWidth = 0;
    if (g_app.config.showMemory) {
        rowOneWidth = MeasureText(dc, L"内存 100%");
    }
    const int customWidth = CustomColumnWidth(dc);
    if (customWidth > 0) {
        if (rowOneWidth > 0) {
            rowOneWidth += kColumnGap;
        }
        rowOneWidth += customWidth;
    }
    return std::max(width, rowOneWidth);
}

int ComputeContentWidth(HDC dc) {
    EnsureFont(g_app.dpi);
    HGDIOBJ oldFont = SelectObject(dc, g_app.font);
    const int speedWidth = SpeedColumnWidth(dc);
    const int infoWidth = InfoColumnWidth(dc);
    int width = kPadding * 2 + speedWidth;
    if (infoWidth > 0) {
        width += kColumnGap + infoWidth;
    }
    SelectObject(dc, oldFont);
    return std::max(width, kMinWidth);
}

void DrawArrow(HDC dc, int x, int y, int width, int height, bool up, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, GetStockObject(NULL_PEN)));

    if (up) {
        POINT points[3] = {
            {x + width / 2, y},
            {x, y + height * 2 / 3},
            {x + width, y + height * 2 / 3},
        };
        Polygon(dc, points, 3);
        RECT stem{x + width / 2 - 1, y + height / 2, x + width / 2 + 2, y + height};
        FillRect(dc, &stem, brush);
    } else {
        POINT points[3] = {
            {x, y + height / 3},
            {x + width, y + height / 3},
            {x + width / 2, y + height},
        };
        Polygon(dc, points, 3);
        RECT stem{x + width / 2 - 1, y, x + width / 2 + 2, y + height / 2};
        FillRect(dc, &stem, brush);
    }

    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void DrawRun(HDC dc, const std::wstring& text, int x, int y, COLORREF color) {
    if (text.empty()) {
        return;
    }
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.c_str(), static_cast<int>(text.size()));
}

bool IsTaskbarUsable() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr || !IsWindowVisible(taskbar)) {
        return false;
    }

    RECT rect{};
    if (!GetWindowRect(taskbar, &rect)) {
        return false;
    }
    if (rect.bottom <= 0 || rect.top >= GetSystemMetrics(SM_CYSCREEN)) {
        return false;
    }

    // Do not hide the widget just because another window is fullscreen.
    // The widget is an owned window above the taskbar, and the user expects
    // the taskbar monitor to stay visible until the taskbar itself disappears.
    return true;
}

COLORREF SampleTaskbarColor() {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr) {
        return g_app.backgroundColor;
    }

    RECT rect{};
    if (!GetWindowRect(taskbar, &rect)) {
        return g_app.backgroundColor;
    }

    struct Bucket {
        COLORREF color;
        int count;
    };
    std::vector<Bucket> buckets;
    HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return g_app.backgroundColor;
    }

    const int width = static_cast<int>(rect.right - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    auto addSample = [&](int x, int y) {
        if (g_app.windowRect.right > g_app.windowRect.left && x >= g_app.windowRect.left &&
            x <= g_app.windowRect.right && y >= g_app.windowRect.top &&
            y <= g_app.windowRect.bottom) {
            return;
        }
        const COLORREF color = GetPixel(screen, x, y);
        if (color == CLR_INVALID) {
            return;
        }
        auto it = std::find_if(buckets.begin(), buckets.end(), [color](const Bucket& item) {
            return item.color == color;
        });
        if (it == buckets.end()) {
            buckets.push_back({color, 1});
        } else {
            ++it->count;
        }
    };

    if (height > width) {
        HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
        RECT trayRect{};
        int bottom = static_cast<int>(rect.bottom) - 16;
        if (tray != nullptr && GetWindowRect(tray, &trayRect)) {
            bottom = trayRect.top - 4;
        }
        const int sampleColumns[] = {static_cast<int>(rect.left) + 6,
                                     static_cast<int>(rect.left) + width / 2,
                                     static_cast<int>(rect.right) - 6};
        for (int y = static_cast<int>(rect.top) + 40; y < bottom; y += 32) {
            for (int x : sampleColumns) {
                addSample(x, y);
            }
        }
    } else {
        const int middle = static_cast<int>(rect.top) + height / 2;
        const int sampleRows[] = {middle, middle - std::max(1, height / 4),
                                  middle + std::max(1, height / 4)};
        for (int x = static_cast<int>(rect.left) + 40; x < rect.right - 16; x += 40) {
            for (int y : sampleRows) {
                addSample(x, y);
            }
        }
    }
    ReleaseDC(nullptr, screen);

    if (buckets.empty()) {
        return g_app.backgroundColor;
    }
    const auto best = std::max_element(buckets.begin(), buckets.end(),
                                       [](const Bucket& a, const Bucket& b) {
                                           return a.count < b.count;
                                       });
    return best->color;
}

void UpdateThemeColors(bool force) {
    const ULONGLONG now = GetTickCount64();
    if (!force && now - g_app.lastThemeSampleTick < 30000) {
        return;
    }
    g_app.lastThemeSampleTick = now;

    COLORREF background = RGB(30, 30, 38);
    if (g_app.config.themeMode == 2) {
        background = RGB(245, 245, 245);
    } else if (g_app.config.themeMode == 3) {
        background = RGB(22, 30, 58);
    } else if (g_app.config.themeMode == 4) {
        background = RGB(24, 38, 34);
    } else if (g_app.config.themeMode == 5) {
        background = RGB(255, 205, 64);
    } else if (g_app.config.themeMode == 6) {
        background = RGB(247, 184, 204);
    } else if (g_app.config.themeMode == 7) {
        background = RGB(118, 83, 173);
    } else if (g_app.config.themeMode == 8) {
        background = RGB(255, 140, 0);
    } else if (g_app.config.themeMode == 9) {
        background = RGB(0, 121, 107);
    } else if (g_app.config.themeMode == 10) {
        background = RGB(136, 14, 45);
    } else if (g_app.config.themeMode == 0) {
        background = SampleTaskbarColor();
    }
    g_app.backgroundColor = background;

    const int luminance =
        (299 * GetRValue(background) + 587 * GetGValue(background) + 114 * GetBValue(background)) /
        1000;
    g_app.lightTheme = luminance > 150;
    if (g_app.lightTheme) {
        g_app.textColor = RGB(32, 32, 32);
        g_app.labelColor = RGB(86, 86, 86);
        g_app.upColor = RGB(20, 130, 60);
        g_app.downColor = RGB(30, 90, 180);
    } else {
        g_app.textColor = RGB(245, 245, 245);
        g_app.labelColor = RGB(196, 196, 212);
        g_app.upColor = RGB(120, 215, 130);
        g_app.downColor = RGB(120, 170, 255);
    }
}

bool UseStackedVerticalLayout(int width) {
    return width < 110;
}

int VerticalLineCount(int width) {
    const bool stacked = UseStackedVerticalLayout(width);
    const bool hasCustom = g_app.config.customEnabled && !g_app.config.customItems.empty();
    if (stacked) {
        int lines = 4;  // up/down speed, each two lines
        if (g_app.config.showCpu) {
            lines += 2;
        }
        if (g_app.config.showMemory) {
            lines += 2;
        }
        if (hasCustom) {
            lines += 3;  // alias, price, change
        }
        return lines;
    }

    int lines = 2;
    if (g_app.config.showCpu) {
        ++lines;
    }
    if (g_app.config.showMemory) {
        ++lines;
    }
    if (hasCustom) {
        ++lines;
    }
    return lines;
}

int ComputeVerticalContentHeight(HDC dc, int width) {
    EnsureFont(g_app.dpi);
    HGDIOBJ oldFont = SelectObject(dc, g_app.font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, oldFont);
    const int lineHeight = metrics.tmHeight + 2;
    return kPadding * 2 + lineHeight * VerticalLineCount(width);
}

RECT TaskbarTrayRect(HWND taskbar, const RECT& taskbarRect) {
    RECT trayRect = taskbarRect;
    HWND tray = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (tray != nullptr) {
        GetWindowRect(tray, &trayRect);
        return trayRect;
    }

    // Windows 11 can expose a slightly different tray layout. Keep a
    // conservative fallback on the taskbar itself instead of a desktop point.
    if ((taskbarRect.bottom - taskbarRect.top) > (taskbarRect.right - taskbarRect.left)) {
        trayRect.top = taskbarRect.bottom - 220;
    } else {
        trayRect.left = taskbarRect.right - 220;
    }
    return trayRect;
}

bool ComputePlacement(int& x, int& y, int& width, int& height, int& dpi) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar == nullptr) {
        return false;
    }

    RECT taskbarRect{};
    if (!GetWindowRect(taskbar, &taskbarRect)) {
        return false;
    }
    const int taskbarWidth = taskbarRect.right - taskbarRect.left;
    const int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
    if (taskbarWidth < 40 || taskbarHeight < 20) {
        return false;
    }

    const RECT trayRect = TaskbarTrayRect(taskbar, taskbarRect);

    dpi = GetDpiForWindow(taskbar);
    if (dpi == 0) {
        dpi = 96;
    }
    EnsureFont(dpi);
    g_app.taskbarVertical = taskbarWidth <= taskbarHeight;

    HDC dc = GetDC(g_app.taskbarHwnd);
    if (dc == nullptr) {
        return false;
    }
    if (g_app.taskbarVertical) {
        width = taskbarWidth;
        height = ComputeVerticalContentHeight(dc, taskbarWidth);
        ReleaseDC(g_app.taskbarHwnd, dc);
        if (height > taskbarHeight - 8) {
            height = taskbarHeight - 8;
        }
        x = taskbarRect.left;
        y = trayRect.top - height - 2;
        if (y < taskbarRect.top) {
            y = taskbarRect.top;
        }
        return true;
    }

    if (taskbarWidth < 220) {
        ReleaseDC(g_app.taskbarHwnd, dc);
        return false;
    }
    width = ComputeContentWidth(dc);
    ReleaseDC(g_app.taskbarHwnd, dc);
    height = taskbarHeight;
    if (height < 20) {
        height = 30;
    }

    x = trayRect.left - width - 2;
    y = taskbarRect.top;
    if (x < taskbarRect.left + 36) {
        x = taskbarRect.left + 36;
    }
    return true;
}

bool ComputeSecondaryPlacement(HWND taskbar, HWND window, int& x, int& y, int& width,
                               int& height, bool& vertical) {
    RECT taskbarRect{};
    if (!GetWindowRect(taskbar, &taskbarRect)) {
        return false;
    }

    const int taskbarWidth = taskbarRect.right - taskbarRect.left;
    const int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
    if (taskbarWidth < 40 || taskbarHeight < 20) {
        return false;
    }

    const RECT trayRect = TaskbarTrayRect(taskbar, taskbarRect);

    vertical = taskbarWidth <= taskbarHeight;
    HDC dc = GetDC(window);
    if (dc == nullptr) {
        return false;
    }

    if (vertical) {
        width = taskbarWidth;
        height = ComputeVerticalContentHeight(dc, taskbarWidth);
        if (height > taskbarHeight - 8) {
            height = taskbarHeight - 8;
        }
        x = taskbarRect.left;
        y = trayRect.top - height - 2;
        if (y < taskbarRect.top) {
            y = taskbarRect.top;
        }
    } else {
        if (taskbarWidth < 220) {
            ReleaseDC(window, dc);
            return false;
        }
        width = ComputeContentWidth(dc);
        height = taskbarHeight;
        x = trayRect.left - width - 2;
        y = taskbarRect.top;
        if (x < taskbarRect.left + 36) {
            x = taskbarRect.left + 36;
        }
    }

    ReleaseDC(window, dc);
    return true;
}

LRESULT CALLBACK SecondaryTaskbarWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto findSurface = [hwnd]() -> SecondaryTaskbarSurface* {
        for (auto& surface : g_secondaryTaskbars) {
            if (surface.window == hwnd) {
                return &surface;
            }
        }
        return nullptr;
    };

    switch (message) {
    case WM_PAINT: {
        SecondaryTaskbarSurface* surface = findSurface();
        if (surface == nullptr) {
            return 0;
        }
        const bool oldVertical = g_app.taskbarVertical;
        g_app.taskbarVertical = surface->vertical;
        PaintTaskbar(hwnd);
        g_app.taskbarVertical = oldVertical;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (!g_hoverVisible) {
            SetTimer(hwnd, kTimerHoverShow, 300, nullptr);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kTimerHoverShow) {
            KillTimer(hwnd, kTimerHoverShow);
            if (SecondaryTaskbarSurface* surface = findSurface()) {
                ShowHoverTipForSurface(surface->taskbar, surface->window);
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        if (g_hoverTaskbar != nullptr) {
            wchar_t className[128]{};
            GetClassNameW(g_hoverTaskbar, className, ARRAYSIZE(className));
            if (wcscmp(className, L"Shell_SecondaryTrayWnd") == 0) {
                HideHoverTip();
            }
        }
        return 0;
    case WM_RBUTTONUP: {
        POINT point{};
        GetCursorPos(&point);
        ShowAppMenu(hwnd, point);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

BOOL CALLBACK EnumSecondaryTaskbarProc(HWND hwnd, LPARAM lParam) {
    wchar_t className[128]{};
    if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0) {
        return TRUE;
    }
    if (wcscmp(className, L"Shell_SecondaryTrayWnd") == 0 && IsWindowVisible(hwnd)) {
        reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hwnd);
    }
    return TRUE;
}

bool RegisterSecondaryTaskbarClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = SecondaryTaskbarWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kSecondaryTaskbarClass;
    return RegisterClassExW(&windowClass) != 0 ||
           GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DestroySecondaryTaskbar(SecondaryTaskbarSurface& surface) {
    if (surface.window != nullptr && IsWindow(surface.window)) {
        DestroyWindow(surface.window);
    }
    surface.window = nullptr;
    surface.taskbar = nullptr;
}

void RefreshSecondaryTaskbars(bool force) {
    std::vector<HWND> taskbars;
    EnumWindows(EnumSecondaryTaskbarProc, reinterpret_cast<LPARAM>(&taskbars));

    for (auto& surface : g_secondaryTaskbars) {
        if (surface.taskbar == nullptr ||
            std::find(taskbars.begin(), taskbars.end(), surface.taskbar) == taskbars.end()) {
            DestroySecondaryTaskbar(surface);
        }
    }
    g_secondaryTaskbars.erase(
        std::remove_if(g_secondaryTaskbars.begin(), g_secondaryTaskbars.end(),
                       [](const SecondaryTaskbarSurface& surface) {
                           return surface.window == nullptr;
                       }),
        g_secondaryTaskbars.end());

    if (!RegisterSecondaryTaskbarClass()) {
        return;
    }

    for (HWND taskbar : taskbars) {
        auto existing = std::find_if(g_secondaryTaskbars.begin(), g_secondaryTaskbars.end(),
                                     [taskbar](const SecondaryTaskbarSurface& surface) {
                                         return surface.taskbar == taskbar;
                                     });
        if (existing == g_secondaryTaskbars.end()) {
            HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                          kSecondaryTaskbarClass, L"", WS_POPUP, 0, 0, 10, 10,
                                          nullptr, nullptr, g_app.instance, nullptr);
            if (window == nullptr) {
                continue;
            }
            g_secondaryTaskbars.push_back({window, taskbar, false});
            existing = std::prev(g_secondaryTaskbars.end());
        }

        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        bool vertical = false;
        if (!ComputeSecondaryPlacement(taskbar, existing->window, x, y, width, height, vertical)) {
            DestroySecondaryTaskbar(*existing);
            continue;
        }

        RECT current{};
        GetWindowRect(existing->window, &current);
        const bool changed = force || current.left != x || current.top != y ||
                             current.right != x + width || current.bottom != y + height ||
                             existing->vertical != vertical;
        existing->vertical = vertical;
        if (changed) {
            SetWindowPos(existing->window, HWND_TOPMOST, x, y, width, height,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        InvalidateRect(existing->window, nullptr, FALSE);
    }

    g_secondaryTaskbars.erase(
        std::remove_if(g_secondaryTaskbars.begin(), g_secondaryTaskbars.end(),
                       [](const SecondaryTaskbarSurface& surface) {
                           return surface.window == nullptr;
                       }),
        g_secondaryTaskbars.end());
}

void DestroySecondaryTaskbars() {
    for (auto& surface : g_secondaryTaskbars) {
        DestroySecondaryTaskbar(surface);
    }
    g_secondaryTaskbars.clear();
    UnregisterClassW(kSecondaryTaskbarClass, g_app.instance);
}

void PaintTaskbar(HWND hwnd) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd, &paint);
    if (dc == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        EndPaint(hwnd, &paint);
        return;
    }

    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(g_app.backgroundColor);
    FillRect(memory, &client, background);
    DeleteObject(background);

    SetBkMode(memory, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(memory, g_app.font);

    TEXTMETRICW metrics{};
    GetTextMetricsW(memory, &metrics);
    const auto& snapshot = g_app.sampler.snapshot();

    if (g_app.taskbarVertical) {
        const int lineHeight = metrics.tmHeight + 2;
        const bool stacked = UseStackedVerticalLayout(width);
        int y = kPadding;
        auto drawLine = [&](const std::wstring& text, COLORREF color, bool arrow, bool up) {
            if (arrow) {
                DrawArrow(memory, kPadding, y + (lineHeight - 9) / 2, 8, 9, up,
                          up ? g_app.upColor : g_app.downColor);
            }
            const int textX = arrow ? kPadding + kArrowWidth : kPadding;
            RECT lineRect{textX, y, width - kPadding, y + lineHeight};
            SetTextColor(memory, color);
            DrawTextW(memory, text.c_str(), -1, &lineRect,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
            y += lineHeight;
        };
        auto compactSpeed = [](const std::wstring& text) {
            std::wstring compact;
            compact.reserve(text.size());
            for (wchar_t ch : text) {
                if (ch != L' ') {
                    compact.push_back(ch);
                }
            }
            return compact;
        };

        if (stacked) {
            drawLine(L"", g_app.textColor, true, true);
            drawLine(compactSpeed(FormatSpeed(snapshot.upBps)), g_app.textColor, false, false);
            drawLine(L"", g_app.textColor, true, false);
            drawLine(compactSpeed(FormatSpeed(snapshot.downBps)), g_app.textColor, false, false);
            if (g_app.config.showCpu) {
                drawLine(L"CPU", g_app.labelColor, false, false);
                drawLine(FormatPercent(snapshot.cpuPercent), g_app.textColor, false, false);
            }
            if (g_app.config.showMemory) {
                drawLine(L"内存", g_app.labelColor, false, false);
                drawLine(FormatPercent(snapshot.memPercent), g_app.textColor, false, false);
            }
            if (g_app.config.customEnabled) {
                std::wstring alias = g_app.config.customPrimary;
                std::wstring price = L"--";
                std::wstring change = L"--";
                const std::vector<QuoteSnapshot> quotes = g_app.quote.Snapshot();
                const QuoteSnapshot* quote = FindPrimaryQuote(quotes);
                if (quote != nullptr) {
                    alias = quote->alias;
                    if (quote->valid) {
                        price = FormatQuotePrice(quote->price);
                        change = FormatQuoteChange(quote->changePercent);
                    }
                }
                drawLine(alias, g_app.labelColor, false, false);
                drawLine(price, g_app.textColor, false, false);
                drawLine(change, g_app.textColor, false, false);
            }
        } else {
            drawLine(FormatSpeed(snapshot.upBps), g_app.textColor, true, true);
            drawLine(FormatSpeed(snapshot.downBps), g_app.textColor, true, false);
            if (g_app.config.showCpu) {
                drawLine(L"CPU " + FormatPercent(snapshot.cpuPercent), g_app.textColor, false,
                         false);
            }
            if (g_app.config.showMemory) {
                drawLine(L"内存 " + FormatPercent(snapshot.memPercent), g_app.textColor, false,
                         false);
            }
            if (g_app.config.customEnabled) {
                const std::wstring quote = PrimaryQuoteText();
                if (!quote.empty()) {
                    drawLine(quote, g_app.labelColor, false, false);
                }
            }
        }
    } else {
        const int textHeight = metrics.tmHeight;
        const int rowHeight = height / 2;
        const int speedWidth = SpeedColumnWidth(memory);
        const int infoX = kPadding + speedWidth + kColumnGap;

        for (int row = 0; row < 2; ++row) {
            const int rowTop = row * rowHeight;
            int textY = rowTop + (rowHeight - textHeight) / 2;
            if (textY < rowTop) {
                textY = rowTop;
            }

            const bool up = row == 0;
            DrawArrow(memory, kPadding, rowTop + (rowHeight - 9) / 2, 8, 9, up,
                      up ? g_app.upColor : g_app.downColor);

            const std::wstring speed =
                up ? FormatSpeed(snapshot.upBps) : FormatSpeed(snapshot.downBps);
            DrawRun(memory, speed, kPadding + kArrowWidth, textY, g_app.textColor);

            if (row == 0) {
                if (g_app.config.showCpu) {
                    const std::wstring cpuValue = FormatPercent(snapshot.cpuPercent);
                    DrawRun(memory, L"CPU", infoX, textY, g_app.labelColor);
                    const int labelWidth = MeasureText(memory, L"CPU");
                    DrawRun(memory, cpuValue, infoX + labelWidth + 2, textY, g_app.textColor);
                }
            } else {
                int cursorX = infoX;
                if (g_app.config.showMemory) {
                    const std::wstring memoryValue = FormatPercent(snapshot.memPercent);
                    DrawRun(memory, L"内存", cursorX, textY, g_app.labelColor);
                    const int labelWidth = MeasureText(memory, L"内存");
                    DrawRun(memory, memoryValue, cursorX + labelWidth + 2, textY, g_app.textColor);
                    cursorX += labelWidth + 2 + MeasureText(memory, memoryValue) + kColumnGap;
                }
                if (g_app.config.customEnabled) {
                    const std::wstring quote = PrimaryQuoteText();
                    if (!quote.empty()) {
                        DrawRun(memory, quote, cursorX, textY, g_app.labelColor);
                    }
                }
            }
        }
    }

    SelectObject(memory, oldFont);
    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(hwnd, &paint);
}

LRESULT CALLBACK TaskbarWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_HOTKEY:
        if (wParam == kHotkeySettings) {
            ShowCustomSettings(hwnd);
            return 0;
        }
        return 0;

    case WM_QUOTE_UPDATED:
        RefreshTaskbarWindow(false);
        if (g_hoverVisible) {
            UpdateHoverTip();
        }
        return 0;

    case WM_SHOW_SETTINGS:
        ShowCustomSettings(hwnd);
        return 0;

    case WM_TIMER:
        if (wParam == kTimerZOrder) {
            EnsureAboveTaskbar();
            HideHoverTipIfLeft();
            return 0;
        }
        if (wParam == kTimerHoverShow) {
            KillTimer(hwnd, kTimerHoverShow);
            g_hoverTaskbar = nullptr;
            g_hoverSurfaceRect = g_app.windowRect;
            ShowHoverTip();
            return 0;
        }
        if (wParam == kTimerThemeRefresh) {
            KillTimer(hwnd, kTimerThemeRefresh);
            RefreshTaskbarWindow(true);
            return 0;
        }
        if (wParam == TIMER_SAMPLE) {
            g_app.sampler.Tick();
            const auto& snapshot = g_app.sampler.snapshot();
            g_app.history.Add(snapshot.upBytesDelta, snapshot.downBytesDelta);

            const ULONGLONG now = GetTickCount64();
            if (g_app.history.dirty() && now - g_app.lastHistorySaveTick >= 60000) {
                g_app.history.Save();
                g_app.lastHistorySaveTick = now;
            }
            RefreshTaskbarWindow(false);
        }
        return 0;

    case WM_PAINT:
        PaintTaskbar(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        if (!g_hoverVisible) {
            SetTimer(hwnd, kTimerHoverShow, 300, nullptr);
        }
        return 0;

    case WM_RBUTTONUP: {
        HideHoverTip();
        POINT point{};
        GetCursorPos(&point);
        ShowAppMenu(hwnd, point);
        return 0;
    }

    case WM_LBUTTONDBLCLK:
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        return 0;

    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        // Windows broadcasts several settings messages for one color change.
        // Debounce them so theme sampling runs once instead of stalling the UI.
        g_app.lastThemeSampleTick = 0;
        SetTimer(hwnd, kTimerThemeRefresh, 250, nullptr);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        g_app.lastThemeSampleTick = 0;
        RefreshTaskbarWindow(true);
        return 0;

    case WM_POWERBROADCAST:
        if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
            g_app.history.Save();
            g_app.sampler.Reset();
            g_app.lastThemeSampleTick = 0;
            RefreshTaskbarWindow(true);
        }
        return TRUE;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != 0) {
            g_app.history.Save();
        }
        return 0;

    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT point{};
            GetCursorPos(&point);
            ShowAppMenu(hwnd, point);
            break;
        }
        case WM_LBUTTONDBLCLK:
            ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        default:
            break;
        }
        return 0;

    case WM_SHOWMENU: {
        HideHoverTip();
        POINT point{};
        GetCursorPos(&point);
        ShowAppMenu(hwnd, point);
        return 0;
    }

    case WM_SHOWHISTORY:
        HideHoverTip();
        ShowHistoryWindow(hwnd);
        return 0;

    case WM_SHOWPROCESS:
        HideHoverTip();
        ShowProcessTrafficWindow(hwnd);
        return 0;

    case WM_WTSSESSION_CHANGE:
        if (wParam == WTS_SESSION_LOCK) {
            g_app.sessionLocked = true;
            HideHoverTip();
            AppendAppLog(L"session locked; sampling paused");
        } else if (wParam == WTS_SESSION_UNLOCK) {
            g_app.sessionLocked = false;
            g_app.lastThemeSampleTick = 0;
            g_app.sampler.Reset();
            g_app.sampler.Tick();
            AppendAppLog(L"session unlocked; sampling resumed");
            RefreshTaskbarWindow(true);
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_SAMPLE);
        KillTimer(hwnd, kTimerZOrder);
        KillTimer(hwnd, kTimerHoverShow);
        KillTimer(hwnd, kTimerThemeRefresh);
        DestroyHoverTip();
        if (g_app.taskbarHwnd == hwnd) {
            g_app.taskbarHwnd = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

}  // namespace

bool CreateTaskbarWindow(HINSTANCE instance) {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = TaskbarWndProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    g_app.taskbarHwnd =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kWindowClass, kAppName, WS_POPUP, 0, 0,
                        10, 10, nullptr, nullptr, instance, nullptr);
    if (g_app.taskbarHwnd == nullptr) {
        return false;
    }

    // Keep the popup in the taskbar's owned-window band. An owned window stays
    // above its owner during taskbar activation, so clicking the taskbar does
    // not require a repeated HWND_TOPMOST fight.
    if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr)) {
        SetWindowLongPtrW(g_app.taskbarHwnd, GWLP_HWNDPARENT,
                          reinterpret_cast<LONG_PTR>(taskbar));
    }

    WTSRegisterSessionNotification(g_app.taskbarHwnd, NOTIFY_FOR_THIS_SESSION);
    EnsureFont(96, true);
    g_app.sampler.SetPreferredAdapter(g_app.config.adapterLuid);
    g_app.sampler.Tick();
    SetTimer(g_app.taskbarHwnd, TIMER_SAMPLE, static_cast<UINT>(g_app.config.intervalMs), nullptr);
    SetTimer(g_app.taskbarHwnd, kTimerZOrder, 150, nullptr);
    CreateHoverTipWindow();

    ConfigureQuoteService();
    g_app.quote.Start(g_app.taskbarHwnd);
    if (!g_app.safeMode) {
        g_app.plugins.Load();
    }

    if (!RegisterHotKey(g_app.taskbarHwnd, kHotkeySettings, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                        'S')) {
        RegisterHotKey(g_app.taskbarHwnd, kHotkeySettings, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                       'M');
    }


    RefreshTaskbarWindow(true);
    return true;
}

void DestroyTaskbarWindow() {
    DestroyProcessTrafficWindow();
    g_app.plugins.Unload();
    g_app.processTraffic.Stop();
    AppendAppLog(L"TrafficBar stopping");
    g_app.quote.Stop();
    if (g_app.taskbarHwnd != nullptr) {
        WTSUnRegisterSessionNotification(g_app.taskbarHwnd);
        UnregisterHotKey(g_app.taskbarHwnd, kHotkeySettings);
    }
    DestroyCustomSettings();
    DestroyHoverTip();
    DestroySecondaryTaskbars();
    if (g_app.taskbarHwnd != nullptr) {
        KillTimer(g_app.taskbarHwnd, TIMER_SAMPLE);
        KillTimer(g_app.taskbarHwnd, kTimerZOrder);
        KillTimer(g_app.taskbarHwnd, kTimerHoverShow);
        DestroyWindow(g_app.taskbarHwnd);
        g_app.taskbarHwnd = nullptr;
    }
    if (g_app.font != nullptr) {
        DeleteObject(g_app.font);
        g_app.font = nullptr;
    }
    UnregisterClassW(kHoverTipClass, g_app.instance);
    UnregisterClassW(kWindowClass, g_app.instance);
}

void RefreshTaskbarWindow(bool force) {
    if (g_app.taskbarHwnd == nullptr || g_app.sessionLocked) {
        return;
    }

    if (!IsTaskbarUsable()) {
        HideHoverTip();
        if (IsWindowVisible(g_app.taskbarHwnd)) {
            ShowWindow(g_app.taskbarHwnd, SW_HIDE);
        }
        g_app.taskbarVisible = false;
        return;
    }

    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int dpi = 96;
    if (!ComputePlacement(x, y, width, height, dpi)) {
        HideHoverTip();
        if (IsWindowVisible(g_app.taskbarHwnd)) {
            ShowWindow(g_app.taskbarHwnd, SW_HIDE);
        }
        g_app.taskbarVisible = false;
        return;
    }

    bool changed = force || width != g_app.width || height != g_app.height;
    RECT current{};
    GetWindowRect(g_app.taskbarHwnd, &current);
    if (current.left != x || current.top != y || current.right != x + width ||
        current.bottom != y + height) {
        changed = true;
    }

    if (changed) {
        // Theme sampling is expensive because it reads many screen pixels.
        // Only do it when an explicit theme/display event marked the cache dirty.
        if (force && g_app.lastThemeSampleTick == 0) {
            UpdateThemeColors(true);
        }
        SetWindowPos(g_app.taskbarHwnd, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_app.width = width;
        g_app.height = height;
        g_app.windowRect = {x, y, x + width, y + height};
        g_app.taskbarVisible = true;
    }

    if (!IsWindowVisible(g_app.taskbarHwnd)) {
        ShowWindow(g_app.taskbarHwnd, SW_SHOWNOACTIVATE);
        g_app.taskbarVisible = true;
    }

    InvalidateRect(g_app.taskbarHwnd, nullptr, FALSE);
    UpdateHoverTip();
    UpdateTrayTip();
    RefreshSecondaryTaskbars(force);

    static ULONGLONG lastHeartbeatTick = 0;
    const ULONGLONG heartbeatNow = GetTickCount64();
    if (lastHeartbeatTick == 0 || heartbeatNow - lastHeartbeatTick >= 60000) {
        lastHeartbeatTick = heartbeatNow;
        AppendAppLog(L"heartbeat");
    }
}
