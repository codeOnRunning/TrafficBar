#include "app.h"

#include <cwchar>

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr int IDC_HISTORY_LIST = 2001;
constexpr int IDC_HISTORY_CLOSE = 2002;
constexpr int IDC_HISTORY_CLEAR = 2003;
constexpr int IDC_HISTORY_MODE_BASE = 2004;

constexpr const wchar_t* kHistoryClass = L"TrafficBar.HistoryWindow";

HWND g_historyWindow = nullptr;
HWND g_summaryLabel = nullptr;
HWND g_modeLabel = nullptr;
HWND g_modeButtons[3]{};
HWND g_historyList = nullptr;
HWND g_noteLabel = nullptr;
HWND g_clearButton = nullptr;
HWND g_closeButton = nullptr;
HFONT g_historyFont = nullptr;
HBRUSH g_historyBrush = nullptr;
HBRUSH g_historyListBrush = nullptr;
COLORREF g_historyTextColor = RGB(30, 30, 30);
COLORREF g_historyLabelColor = RGB(80, 80, 80);
COLORREF g_historyListBackground = RGB(255, 255, 255);
COLORREF g_historyHeaderBackground = RGB(240, 240, 240);
COLORREF g_historyButtonColor = RGB(238, 238, 238);
COLORREF g_historyButtonBorder = RGB(180, 180, 180);
RECT g_historyHeaderRect{};
int g_historyMode = 0;

std::wstring DateText(int key) {
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04d-%02d-%02d", key / 10000, (key / 100) % 100, key % 100);
    return buffer;
}

std::wstring PeriodText(int key, int mode) {
    wchar_t buffer[32]{};
    if (mode == 1) {
        swprintf_s(buffer, L"%04d-%02d", key / 100, key % 100);
    } else if (mode == 2) {
        swprintf_s(buffer, L"%04d", key);
    } else {
        return DateText(key);
    }
    return buffer;
}

struct HistoryRow {
    std::wstring label;
    DayTraffic traffic;
};

std::vector<HistoryRow> BuildHistoryRows(int mode) {
    std::map<int, DayTraffic> grouped;
    for (const auto& [key, record] : g_app.history.days()) {
        int groupKey = key;
        if (mode == 1) {
            groupKey = key / 100;
        } else if (mode == 2) {
            groupKey = key / 10000;
        }
        DayTraffic& target = grouped[groupKey];
        target.upBytes += record.upBytes;
        target.downBytes += record.downBytes;
    }

    std::vector<HistoryRow> rows;
    rows.reserve(grouped.size());
    for (auto it = grouped.rbegin(); it != grouped.rend(); ++it) {
        rows.push_back({PeriodText(it->first, mode), it->second});
    }
    return rows;
}

void SetControlFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

LRESULT CALLBACK HistoryListSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                          UINT_PTR, DWORD_PTR) {
    if (message == WM_NOTIFY) {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header != nullptr && header->code == NM_CUSTOMDRAW &&
            header->hwndFrom == ListView_GetHeader(hwnd)) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                draw->clrText = g_historyTextColor;
                draw->clrTextBk = g_historyHeaderBackground;
                return CDRF_NEWFONT;
            }
        }
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void HideHistoryHeader();

void EnsureHistoryBrushes() {
    const bool dark = !g_app.lightTheme;
    const COLORREF background = dark ? RGB(30, 30, 38) : RGB(246, 246, 246);
    const COLORREF listBackground = dark ? RGB(36, 36, 46) : RGB(255, 255, 255);
    g_historyTextColor = dark ? RGB(238, 238, 246) : RGB(30, 30, 30);
    g_historyLabelColor = dark ? RGB(180, 180, 195) : RGB(80, 80, 80);
    g_historyListBackground = listBackground;
    g_historyHeaderBackground = dark ? RGB(46, 46, 58) : RGB(240, 240, 240);
    g_historyButtonColor = dark ? RGB(50, 50, 62) : RGB(238, 238, 238);
    g_historyButtonBorder = dark ? RGB(86, 86, 104) : RGB(180, 180, 180);

    if (g_historyBrush != nullptr) {
        DeleteObject(g_historyBrush);
    }
    if (g_historyListBrush != nullptr) {
        DeleteObject(g_historyListBrush);
    }
    g_historyBrush = CreateSolidBrush(background);
    g_historyListBrush = CreateSolidBrush(listBackground);
}

void ApplyHistoryTheme(HWND hwnd) {
    EnsureHistoryBrushes();

    const BOOL darkTitle = g_app.lightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkTitle, sizeof(darkTitle));

    const wchar_t* theme = g_app.lightTheme ? L"Explorer" : L"DarkMode_Explorer";
    SetWindowTheme(g_historyList, theme, nullptr);
    SetWindowTheme(g_historyList, theme, L"ScrollBar");

    ListView_SetBkColor(g_historyList, g_historyListBackground);
    ListView_SetTextBkColor(g_historyList, g_historyListBackground);
    ListView_SetTextColor(g_historyList, g_historyTextColor);
    ListView_SetExtendedListViewStyle(
        g_historyList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
                           (g_app.lightTheme ? LVS_EX_GRIDLINES : 0));

    HideHistoryHeader();
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void DrawHistoryButton(const DRAWITEMSTRUCT* item) {
    if (item == nullptr || item->hwndItem == nullptr) {
        return;
    }

    RECT rect = item->rcItem;
    const bool selectedMode =
        item->CtlID >= IDC_HISTORY_MODE_BASE && item->CtlID < IDC_HISTORY_MODE_BASE + 3 &&
        static_cast<int>(item->CtlID - IDC_HISTORY_MODE_BASE) == g_historyMode;
    COLORREF background = selectedMode
                              ? (g_app.lightTheme ? RGB(218, 218, 218) : RGB(64, 64, 84))
                              : g_historyButtonColor;
    if ((item->itemState & ODS_SELECTED) != 0) {
        background = g_app.lightTheme ? RGB(202, 202, 202) : RGB(54, 54, 72);
    }

    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, selectedMode ? g_historyTextColor
                                                   : g_historyButtonBorder);
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
    SetTextColor(item->hDC, g_historyTextColor);
    HGDIOBJ oldFont = SelectObject(item->hDC, g_historyFont);
    DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, oldFont);
}

void HideHistoryHeader() {
    if (g_historyList == nullptr) {
        return;
    }
    HWND header = ListView_GetHeader(g_historyList);
    if (header != nullptr) {
        SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) & ~WS_VISIBLE);
        ShowWindow(header, SW_HIDE);
    }
}

void DrawHistoryHeader(HDC dc) {
    if (g_historyList == nullptr || g_historyHeaderRect.right <= g_historyHeaderRect.left) {
        return;
    }

    HBRUSH background = CreateSolidBrush(g_historyHeaderBackground);
    FillRect(dc, &g_historyHeaderRect, background);
    DeleteObject(background);

    const wchar_t* titles[] = {g_historyMode == 0 ? L"日期" : g_historyMode == 1 ? L"月份" : L"年份",
                               L"上传", L"下载", L"合计"};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_historyTextColor);
    HGDIOBJ oldFont = SelectObject(dc, g_historyFont);
    int x = g_historyHeaderRect.left;
    for (int i = 0; i < 4; ++i) {
        const int columnWidth = ListView_GetColumnWidth(g_historyList, i);
        RECT textRect{x + 4, g_historyHeaderRect.top, x + columnWidth - 2,
                      g_historyHeaderRect.bottom};
        DrawTextW(dc, titles[i], -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        x += columnWidth;
    }
    SelectObject(dc, oldFont);

    HPEN pen = CreatePen(PS_SOLID, 1, g_historyButtonBorder);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, g_historyHeaderRect.left, g_historyHeaderRect.bottom - 1, nullptr);
    LineTo(dc, g_historyHeaderRect.right, g_historyHeaderRect.bottom - 1);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void LayoutHistoryWindow(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 16;
    const int summaryHeight = 78;
    const int modeRowHeight = 32;
    const int buttonHeight = 30;
    const int noteHeight = 22;

    const int contentWidth = width - margin * 2;
    if (g_summaryLabel != nullptr) {
        MoveWindow(g_summaryLabel, margin, 12, contentWidth, summaryHeight, TRUE);
    }

    const int modeTop = 12 + summaryHeight + 4;
    if (g_modeLabel != nullptr) {
        MoveWindow(g_modeLabel, margin, modeTop + 4, 44, 24, TRUE);
    }
    for (int i = 0; i < 3; ++i) {
        if (g_modeButtons[i] != nullptr) {
            MoveWindow(g_modeButtons[i], margin + 46 + i * 72, modeTop, 66, 30, TRUE);
        }
    }

    const int listTop = modeTop + modeRowHeight + 6;
    const int headerHeight = 26;
    const int bottomArea = buttonHeight + noteHeight + 18;
    const int listHeight = height - listTop - bottomArea;
    g_historyHeaderRect = {margin, listTop, width - margin, listTop + headerHeight};
    if (g_historyList != nullptr) {
        MoveWindow(g_historyList, margin, listTop + headerHeight, contentWidth,
                   std::max(60, listHeight - headerHeight), TRUE);
    }

    const int buttonY = height - margin - buttonHeight;
    if (g_clearButton != nullptr) {
        MoveWindow(g_clearButton, margin, buttonY, 100, buttonHeight, TRUE);
    }
    if (g_closeButton != nullptr) {
        MoveWindow(g_closeButton, width - margin - 84, buttonY, 84, buttonHeight, TRUE);
    }
    if (g_noteLabel != nullptr) {
        MoveWindow(g_noteLabel, margin, buttonY - noteHeight - 6, contentWidth - 210, noteHeight,
                   TRUE);
    }
    HideHistoryHeader();
}

void RefreshHistoryContents() {
    if (g_summaryLabel == nullptr || g_historyList == nullptr) {
        return;
    }

    const DayTraffic today = g_app.history.Today();
    const DayTraffic month = g_app.history.ThisMonth();
    const DayTraffic year = g_app.history.ThisYear();
    const DayTraffic total = g_app.history.Total();

    wchar_t summary[1024]{};
    if (g_historyMode == 0) {
        swprintf_s(summary,
                   L"今日    ↑ %ls    ↓ %ls    合计 %ls\r\n"
                   L"本月    ↑ %ls    ↓ %ls    合计 %ls\r\n"
                   L"总计    ↑ %ls    ↓ %ls    合计 %ls",
                   FormatBytes(today.upBytes).c_str(), FormatBytes(today.downBytes).c_str(),
                   FormatBytes(today.Total()).c_str(), FormatBytes(month.upBytes).c_str(),
                   FormatBytes(month.downBytes).c_str(), FormatBytes(month.Total()).c_str(),
                   FormatBytes(total.upBytes).c_str(), FormatBytes(total.downBytes).c_str(),
                   FormatBytes(total.Total()).c_str());
    } else if (g_historyMode == 1) {
        swprintf_s(summary,
                   L"本月    ↑ %ls    ↓ %ls    合计 %ls\r\n"
                   L"今年    ↑ %ls    ↓ %ls    合计 %ls\r\n"
                   L"总计    ↑ %ls    ↓ %ls    合计 %ls",
                   FormatBytes(month.upBytes).c_str(), FormatBytes(month.downBytes).c_str(),
                   FormatBytes(month.Total()).c_str(), FormatBytes(year.upBytes).c_str(),
                   FormatBytes(year.downBytes).c_str(), FormatBytes(year.Total()).c_str(),
                   FormatBytes(total.upBytes).c_str(), FormatBytes(total.downBytes).c_str(),
                   FormatBytes(total.Total()).c_str());
    } else {
        swprintf_s(summary,
                   L"今年    ↑ %ls    ↓ %ls    合计 %ls\r\n"
                   L"总计    ↑ %ls    ↓ %ls    合计 %ls",
                   FormatBytes(year.upBytes).c_str(), FormatBytes(year.downBytes).c_str(),
                   FormatBytes(year.Total()).c_str(), FormatBytes(total.upBytes).c_str(),
                   FormatBytes(total.downBytes).c_str(), FormatBytes(total.Total()).c_str());
    }
    SetWindowTextW(g_summaryLabel, summary);

    const wchar_t* firstColumn = g_historyMode == 0 ? L"日期"
                                : g_historyMode == 1 ? L"月份"
                                                     : L"年份";
    LVCOLUMNW title{};
    title.mask = LVCF_TEXT;
    title.pszText = const_cast<wchar_t*>(firstColumn);
    ListView_SetColumn(g_historyList, 0, &title);

    ListView_DeleteAllItems(g_historyList);

    const std::vector<HistoryRow> rows = BuildHistoryRows(g_historyMode);
    int row = 0;
    for (const auto& record : rows) {
        const std::wstring up = FormatBytes(record.traffic.upBytes);
        const std::wstring down = FormatBytes(record.traffic.downBytes);
        const std::wstring sum = FormatBytes(record.traffic.Total());

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = const_cast<wchar_t*>(record.label.c_str());
        const int index = ListView_InsertItem(g_historyList, &item);
        if (index < 0) {
            break;
        }
        ListView_SetItemText(g_historyList, index, 1, const_cast<wchar_t*>(up.c_str()));
        ListView_SetItemText(g_historyList, index, 2, const_cast<wchar_t*>(down.c_str()));
        ListView_SetItemText(g_historyList, index, 3, const_cast<wchar_t*>(sum.c_str()));
        ++row;
    }

    if (row == 0) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = 0;
        item.pszText = const_cast<wchar_t*>(L"暂无记录");
        ListView_InsertItem(g_historyList, &item);
    }
    HideHistoryHeader();
}

void CreateHistoryControls(HWND hwnd) {
    const HINSTANCE instance = g_app.instance;
    g_historyFont = CreateFontW(-MulDiv(10, g_app.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                L"Microsoft YaHei UI");

    g_summaryLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     0, 0, 0, 0, hwnd, nullptr, instance, nullptr);

    g_modeLabel = CreateWindowExW(0, L"STATIC", L"查看", WS_CHILD | WS_VISIBLE,
                                  0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
    const wchar_t* modeNames[] = {L"按日", L"按月", L"按年"};
    for (int i = 0; i < 3; ++i) {
        g_modeButtons[i] = CreateWindowExW(
            0, L"BUTTON", modeNames[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0,
            0, 0, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HISTORY_MODE_BASE + i)), instance,
            nullptr);
    }

    g_historyList = CreateWindowExW(
        0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_NOCOLUMNHEADER |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HISTORY_LIST)), instance,
        nullptr);
    ListView_SetExtendedListViewStyle(
        g_historyList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    SetWindowSubclass(g_historyList, HistoryListSubclassProc, 1, 0);

    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    column.fmt = LVCFMT_LEFT;
    column.cx = 130;
    column.pszText = const_cast<wchar_t*>(L"日期");
    ListView_InsertColumn(g_historyList, 0, &column);
    column.cx = 105;
    column.pszText = const_cast<wchar_t*>(L"上传");
    ListView_InsertColumn(g_historyList, 1, &column);
    column.cx = 105;
    column.pszText = const_cast<wchar_t*>(L"下载");
    ListView_InsertColumn(g_historyList, 2, &column);
    column.cx = 105;
    column.pszText = const_cast<wchar_t*>(L"合计");
    ListView_InsertColumn(g_historyList, 3, &column);

    g_noteLabel = CreateWindowExW(0, L"STATIC", L"统计自本程序运行起，关闭期间不计。",
                                  WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, instance,
                                  nullptr);

    g_clearButton = CreateWindowExW(0, L"BUTTON", L"清空历史",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                    hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HISTORY_CLEAR)),
                                    instance, nullptr);
    g_closeButton = CreateWindowExW(0, L"BUTTON", L"关闭",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
                                    hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HISTORY_CLOSE)),
                                    instance, nullptr);

    if (g_historyFont != nullptr) {
        SetControlFont(g_summaryLabel, g_historyFont);
        SetControlFont(g_modeLabel, g_historyFont);
        for (HWND button : g_modeButtons) {
            SetControlFont(button, g_historyFont);
        }
        SetControlFont(g_historyList, g_historyFont);
        SetControlFont(g_noteLabel, g_historyFont);
        SetControlFont(g_clearButton, g_historyFont);
        SetControlFont(g_closeButton, g_historyFont);
    }

    RefreshHistoryContents();
    LayoutHistoryWindow(hwnd);
    ApplyHistoryTheme(hwnd);
}

LRESULT CALLBACK HistoryWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateHistoryControls(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        if (g_historyBrush == nullptr) {
            EnsureHistoryBrushes();
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, g_historyBrush);
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, g_historyBrush);
        DrawHistoryHeader(dc);
        EndPaint(hwnd, &paint);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_historyLabelColor);
        return reinterpret_cast<LRESULT>(g_historyBrush);

    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wParam), g_historyListBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_historyTextColor);
        return reinterpret_cast<LRESULT>(g_historyListBrush);

    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_historyBrush);

    case WM_DRAWITEM:
        DrawHistoryButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (header != nullptr && header->code == NM_CUSTOMDRAW && g_historyList != nullptr &&
            header->hwndFrom == ListView_GetHeader(g_historyList)) {
            auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
            if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                draw->clrText = g_historyTextColor;
                draw->clrTextBk = g_historyHeaderBackground;
                return CDRF_NEWFONT;
            }
        }
        break;
    }

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        g_app.lastThemeSampleTick = 0;
        RefreshTaskbarWindow(true);
        ApplyHistoryTheme(hwnd);
        return 0;

    case WM_SIZE:
        LayoutHistoryWindow(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 540;
        info->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) >= IDC_HISTORY_MODE_BASE &&
            LOWORD(wParam) < IDC_HISTORY_MODE_BASE + 3) {
            g_historyMode = static_cast<int>(LOWORD(wParam) - IDC_HISTORY_MODE_BASE);
            RefreshHistoryContents();
            InvalidateRect(hwnd, &g_historyHeaderRect, TRUE);
            for (HWND button : g_modeButtons) {
                InvalidateRect(button, nullptr, TRUE);
            }
            return 0;
        }
        switch (LOWORD(wParam)) {
        case IDC_HISTORY_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case IDC_HISTORY_CLEAR:
            if (MessageBoxW(hwnd, L"确定清空所有历史流量记录？此操作不可撤销。", L"清空历史",
                            MB_YESNO | MB_ICONWARNING) == IDYES) {
                g_app.history.Clear();
                g_app.history.Save();
                RefreshHistoryContents();
            }
            return 0;
        default:
            break;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_historyList != nullptr) {
            RemoveWindowSubclass(g_historyList, HistoryListSubclassProc, 1);
        }
        g_historyWindow = nullptr;
        g_summaryLabel = nullptr;
        g_modeLabel = nullptr;
        for (HWND& button : g_modeButtons) {
            button = nullptr;
        }
        g_historyList = nullptr;
        g_noteLabel = nullptr;
        g_clearButton = nullptr;
        g_closeButton = nullptr;
        if (g_historyFont != nullptr) {
            DeleteObject(g_historyFont);
            g_historyFont = nullptr;
        }
        if (g_historyBrush != nullptr) {
            DeleteObject(g_historyBrush);
            g_historyBrush = nullptr;
        }
        if (g_historyListBrush != nullptr) {
            DeleteObject(g_historyListBrush);
            g_historyListBrush = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void ShowHistoryWindow(HWND owner) {
    (void)owner;
    g_app.history.Save();

    if (g_historyWindow != nullptr && IsWindow(g_historyWindow)) {
        RefreshHistoryContents();
        ShowWindow(g_historyWindow, IsIconic(g_historyWindow) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_historyWindow);
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = HistoryWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kHistoryClass;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    EnsureHistoryBrushes();
    g_historyWindow = CreateWindowExW(
        0, kHistoryClass, L"流量统计 - TrafficBar",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 560,
        480, nullptr, nullptr, g_app.instance, nullptr);
    if (g_historyWindow == nullptr) {
        return;
    }

    RECT workArea{};
    RECT windowRect{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        GetWindowRect(g_historyWindow, &windowRect)) {
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        SetWindowPos(g_historyWindow, nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(g_historyWindow, IsIconic(g_historyWindow) ? SW_RESTORE : SW_SHOW);
    UpdateWindow(g_historyWindow);
    SetForegroundWindow(g_historyWindow);
}

void RefreshHistoryWindow() {
    if (g_historyWindow != nullptr && IsWindow(g_historyWindow)) {
        RefreshHistoryContents();
    }
}

void DestroyHistoryWindow() {
    if (g_historyWindow != nullptr && IsWindow(g_historyWindow)) {
        DestroyWindow(g_historyWindow);
    }
    g_historyWindow = nullptr;
}
