#include "app.h"

#include <cstdlib>
#include <cwchar>
#include <cwctype>

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr const wchar_t* kSettingsClass = L"TrafficBar.CustomSettings";

constexpr int IDC_ITEMS = 3001;
constexpr int IDC_PRIMARY = 3002;
constexpr int IDC_REFRESH = 3003;
constexpr int IDC_SAVE = 3004;
constexpr int IDC_CANCEL = 3005;
constexpr int IDC_STATUS = 3006;

HWND g_settingsWindow = nullptr;
HWND g_itemsLabel = nullptr;
HWND g_itemsEdit = nullptr;
HWND g_primaryLabel = nullptr;
HWND g_primaryEdit = nullptr;
HWND g_refreshLabel = nullptr;
HWND g_refreshEdit = nullptr;
HWND g_statusLabel = nullptr;
HFONT g_settingsFont = nullptr;
HBRUSH g_settingsBrush = nullptr;
HBRUSH g_settingsEditBrush = nullptr;
COLORREF g_settingsTextColor = RGB(30, 30, 30);
COLORREF g_settingsLabelColor = RGB(80, 80, 80);
COLORREF g_settingsEditBackground = RGB(255, 255, 255);
COLORREF g_settingsButtonColor = RGB(238, 238, 238);
COLORREF g_settingsButtonBorder = RGB(180, 180, 180);

std::wstring GetControlText(HWND control) {
    if (control == nullptr) {
        return L"";
    }
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) {
        return L"";
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

std::wstring TrimSettingsText(const std::wstring& text) {
    size_t begin = 0;
    while (begin < text.size() && iswspace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && iswspace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool SameAlias(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (towlower(a[i]) != towlower(b[i])) {
            return false;
        }
    }
    return true;
}

std::wstring FormatItemsForEdit(const std::vector<CustomItem>& items) {
    const std::wstring joined = FormatCustomItems(items);
    std::wstring result;
    result.reserve(joined.size() + 8);
    for (wchar_t ch : joined) {
        if (ch == L';') {
            result += L"\r\n";
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

void SetFont(HWND control) {
    if (g_settingsFont != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_settingsFont), TRUE);
    }
}

void EnsureSettingsBrushes() {
    const bool dark = !g_app.lightTheme;
    const COLORREF background = dark ? RGB(30, 30, 38) : RGB(246, 246, 246);
    const COLORREF editBackground = dark ? RGB(42, 42, 52) : RGB(255, 255, 255);
    g_settingsTextColor = dark ? RGB(238, 238, 246) : RGB(30, 30, 30);
    g_settingsLabelColor = dark ? RGB(180, 180, 195) : RGB(80, 80, 80);
    g_settingsEditBackground = editBackground;
    g_settingsButtonColor = dark ? RGB(50, 50, 62) : RGB(238, 238, 238);
    g_settingsButtonBorder = dark ? RGB(86, 86, 104) : RGB(180, 180, 180);

    if (g_settingsBrush != nullptr) {
        DeleteObject(g_settingsBrush);
    }
    if (g_settingsEditBrush != nullptr) {
        DeleteObject(g_settingsEditBrush);
    }
    g_settingsBrush = CreateSolidBrush(background);
    g_settingsEditBrush = CreateSolidBrush(editBackground);
}

void ApplySettingsTheme(HWND hwnd) {
    EnsureSettingsBrushes();

    const BOOL darkTitle = g_app.lightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkTitle, sizeof(darkTitle));

    // The dark visual style of a multiline EDIT can override the text color
    // while a selection is moving. Keep the multiline items editor on the
    // stable system edit path and supply its colors through WM_CTLCOLOREDIT.
    SetWindowTheme(g_itemsEdit, L"", L"");
    const wchar_t* editTheme = g_app.lightTheme ? L"Explorer" : L"DarkMode_Explorer";
    SetWindowTheme(g_primaryEdit, editTheme, nullptr);
    SetWindowTheme(g_refreshEdit, editTheme, nullptr);

    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void DrawSettingsButton(const DRAWITEMSTRUCT* item) {
    if (item == nullptr || item->hwndItem == nullptr) {
        return;
    }

    RECT rect = item->rcItem;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    COLORREF background = g_settingsButtonColor;
    if (pressed) {
        background = g_app.lightTheme ? RGB(214, 214, 214) : RGB(36, 36, 46);
    }

    HBRUSH brush = CreateSolidBrush(background);
    HPEN pen = CreatePen(PS_SOLID, 1, g_settingsButtonBorder);
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
    SetTextColor(item->hDC, g_settingsTextColor);
    HGDIOBJ oldFont = SelectObject(item->hDC, g_settingsFont);
    DrawTextW(item->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(item->hDC, oldFont);
}

void UpdateItemsHint() {
    if (g_statusLabel == nullptr) {
        return;
    }
    const std::vector<CustomItem> items = ParseCustomItems(GetControlText(g_itemsEdit));
    wchar_t hint[160]{};
    swprintf_s(hint, L"已识别 %zu 项；每行一个“名称=标识”，可滚动查看全部。",
               items.size());
    SetWindowTextW(g_statusLabel, hint);
}

void LayoutSettings(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 16;
    const int labelHeight = 20;
    const int rowHeight = 26;
    const int buttonHeight = 30;
    const int statusHeight = 22;

    const int itemsLabelY = 12;
    const int itemsTop = itemsLabelY + labelHeight + 4;
    const int buttonsY = height - margin - buttonHeight;
    const int statusY = buttonsY - statusHeight - 4;
    const int formY = statusY - rowHeight - 10;
    const int itemsHeight = formY - itemsTop - 8;

    if (g_itemsLabel != nullptr) {
        MoveWindow(g_itemsLabel, margin, itemsLabelY, width - margin * 2, labelHeight, TRUE);
    }
    if (g_itemsEdit != nullptr) {
        MoveWindow(g_itemsEdit, margin, itemsTop, width - margin * 2, std::max(80, itemsHeight),
                   TRUE);
    }
    if (g_primaryLabel != nullptr) {
        MoveWindow(g_primaryLabel, margin, formY, 70, rowHeight, TRUE);
    }
    if (g_primaryEdit != nullptr) {
        MoveWindow(g_primaryEdit, margin + 76, formY, 140, rowHeight, TRUE);
    }
    if (g_refreshLabel != nullptr) {
        MoveWindow(g_refreshLabel, width - margin - 246, formY, 80, rowHeight, TRUE);
    }
    if (g_refreshEdit != nullptr) {
        MoveWindow(g_refreshEdit, width - margin - 160, formY, 160, rowHeight, TRUE);
    }
    if (g_statusLabel != nullptr) {
        MoveWindow(g_statusLabel, margin, statusY, width - margin * 2, statusHeight, TRUE);
    }
    if (GetDlgItem(hwnd, IDC_SAVE) != nullptr) {
        MoveWindow(GetDlgItem(hwnd, IDC_SAVE), width - margin - 180, buttonsY, 84, buttonHeight,
                   TRUE);
    }
    if (GetDlgItem(hwnd, IDC_CANCEL) != nullptr) {
        MoveWindow(GetDlgItem(hwnd, IDC_CANCEL), width - margin - 84, buttonsY, 84, buttonHeight,
                   TRUE);
    }
}

void CreateSettingsControls(HWND hwnd) {
    const HINSTANCE instance = g_app.instance;
    g_settingsFont = CreateFontW(-MulDiv(10, g_app.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                                 FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Microsoft YaHei UI");

    g_itemsLabel = CreateWindowExW(0, L"STATIC", L"项目（每行：名称=标识）",
                                   WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, instance,
                                   nullptr);
    g_itemsEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_ITEMS)), instance,
        nullptr);

    g_primaryLabel = CreateWindowExW(0, L"STATIC", L"主显示", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0,
                                     hwnd, nullptr, instance, nullptr);
    g_primaryEdit = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                                    0, 0, 0, 0, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRIMARY)),
                                    instance, nullptr);

    g_refreshLabel = CreateWindowExW(0, L"STATIC", L"刷新间隔(ms)", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                     0, hwnd, nullptr, instance, nullptr);
    g_refreshEdit = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                                        ES_AUTOHSCROLL | ES_NUMBER,
                                    0, 0, 0, 0, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)),
                                    instance, nullptr);

    g_statusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                    nullptr, instance, nullptr);

    HWND saveButton = CreateWindowExW(0, L"BUTTON", L"保存",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0,
                                      0, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE)),
                                      instance, nullptr);
    HWND cancelButton =
        CreateWindowExW(0, L"BUTTON", L"取消",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, hwnd,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL)), instance,
                        nullptr);

    SetFont(g_itemsLabel);
    SetFont(g_itemsEdit);
    SetFont(g_primaryLabel);
    SetFont(g_primaryEdit);
    SetFont(g_refreshLabel);
    SetFont(g_refreshEdit);
    SetFont(g_statusLabel);
    SetFont(saveButton);
    SetFont(cancelButton);

    SetWindowTextW(g_itemsEdit, FormatItemsForEdit(g_app.config.customItems).c_str());
    SetWindowTextW(g_primaryEdit, g_app.config.customPrimary.c_str());
    wchar_t refresh[32]{};
    swprintf_s(refresh, L"%d", g_app.config.customRefreshMs);
    SetWindowTextW(g_refreshEdit, refresh);
    LayoutSettings(hwnd);
    SendMessageW(g_itemsEdit, EM_SETSEL, 0, 0);
    SendMessageW(g_itemsEdit, EM_SCROLLCARET, 0, 0);
    UpdateItemsHint();
    ApplySettingsTheme(hwnd);
}

void SaveSettings(HWND hwnd) {
    const std::vector<CustomItem> items = ParseCustomItems(GetControlText(g_itemsEdit));
    if (items.empty()) {
        SetWindowTextW(g_statusLabel, L"至少填写一个项目，格式：名称=标识");
        return;
    }

    std::wstring primary = TrimSettingsText(GetControlText(g_primaryEdit));
    bool primaryFound = false;
    for (const auto& item : items) {
        if (SameAlias(item.alias, primary)) {
            primary = item.alias;
            primaryFound = true;
            break;
        }
    }
    if (!primaryFound) {
        primary = items.front().alias;
    }

    int refresh = _wtoi(GetControlText(g_refreshEdit).c_str());
    refresh = std::clamp(refresh, 1000, 60000);

    g_app.config.customItems = items;
    g_app.config.customPrimary = primary;
    g_app.config.customRefreshMs = refresh;
    g_app.config.customEnabled = true;
    SaveConfig();
    g_app.quote.Configure(g_app.config.customItems, g_app.config.customRefreshMs);
    RefreshTaskbarWindow(false);
    DestroyWindow(hwnd);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        CreateSettingsControls(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        if (g_settingsBrush == nullptr) {
            EnsureSettingsBrushes();
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, g_settingsBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_settingsLabelColor);
        return reinterpret_cast<LRESULT>(g_settingsBrush);

    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(wParam), g_settingsEditBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_settingsTextColor);
        return reinterpret_cast<LRESULT>(g_settingsEditBrush);

    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_settingsBrush);

    case WM_DRAWITEM:
        DrawSettingsButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        g_app.lastThemeSampleTick = 0;
        RefreshTaskbarWindow(true);
        ApplySettingsTheme(hwnd);
        return 0;

    case WM_SIZE:
        LayoutSettings(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 520;
        info->ptMinTrackSize.y = 360;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_SAVE:
            SaveSettings(hwnd);
            return 0;
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        case IDC_ITEMS:
            if (HIWORD(wParam) == EN_CHANGE) {
                // The dark multiline edit can otherwise leave stale paint
                // bands after deleting a selection or adding a line break.
                InvalidateRect(g_itemsEdit, nullptr, TRUE);
                UpdateItemsHint();
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
        g_settingsWindow = nullptr;
        g_itemsLabel = nullptr;
        g_itemsEdit = nullptr;
        g_primaryLabel = nullptr;
        g_primaryEdit = nullptr;
        g_refreshLabel = nullptr;
        g_refreshEdit = nullptr;
        g_statusLabel = nullptr;
        if (g_settingsFont != nullptr) {
            DeleteObject(g_settingsFont);
            g_settingsFont = nullptr;
        }
        if (g_settingsBrush != nullptr) {
            DeleteObject(g_settingsBrush);
            g_settingsBrush = nullptr;
        }
        if (g_settingsEditBrush != nullptr) {
            DeleteObject(g_settingsEditBrush);
            g_settingsEditBrush = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void ShowCustomSettings(HWND owner) {
    if (g_settingsWindow != nullptr && IsWindow(g_settingsWindow)) {
        ShowWindow(g_settingsWindow, IsIconic(g_settingsWindow) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_settingsWindow);
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = SettingsWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kSettingsClass;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return;
    }

    EnsureSettingsBrushes();
    g_settingsWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kSettingsClass, L"自定义指标",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 600,
        460, owner, nullptr, g_app.instance, nullptr);
    if (g_settingsWindow == nullptr) {
        return;
    }

    RECT workArea{};
    RECT windowRect{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        GetWindowRect(g_settingsWindow, &windowRect)) {
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        SetWindowPos(g_settingsWindow, nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(g_settingsWindow, SW_SHOW);
    UpdateWindow(g_settingsWindow);
    SetForegroundWindow(g_settingsWindow);
    SetFocus(g_itemsEdit);
    SendMessageW(g_itemsEdit, EM_SETSEL, 0, 0);
    SendMessageW(g_itemsEdit, EM_SCROLLCARET, 0, 0);
}

void DestroyCustomSettings() {
    if (g_settingsWindow != nullptr && IsWindow(g_settingsWindow)) {
        DestroyWindow(g_settingsWindow);
    }
    g_settingsWindow = nullptr;
}
