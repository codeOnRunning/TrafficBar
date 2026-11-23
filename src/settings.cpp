#include "app.h"

#include <cstdlib>
#include <cwchar>

#include <dwmapi.h>
#include <uxtheme.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace {

constexpr const wchar_t* kAppSettingsClass = L"TrafficBar.AppSettings";

constexpr int IDC_OPACITY_TRACK = 4001;
constexpr int IDC_OPACITY_EDIT = 4002;
constexpr int IDC_HOTKEY_EDIT = 4003;
constexpr int IDC_HOTKEY_CLEAR = 4004;
constexpr int IDC_SAVE = 4005;
constexpr int IDC_CANCEL = 4006;
constexpr int IDC_CUSTOM_SETTINGS = 4007;

HWND g_appSettingsWindow = nullptr;
HWND g_opacityLabel = nullptr;
HWND g_opacityTrack = nullptr;
HWND g_opacityEdit = nullptr;
HWND g_opacityPercent = nullptr;
HWND g_opacityHint = nullptr;
HWND g_hotkeyLabel = nullptr;
HWND g_hotkeyEdit = nullptr;
HWND g_hotkeyClear = nullptr;
HWND g_hotkeyHint = nullptr;
HWND g_customButton = nullptr;
HWND g_saveButton = nullptr;
HWND g_cancelButton = nullptr;
HWND g_divider = nullptr;

HFONT g_appSettingsFont = nullptr;
HBRUSH g_appSettingsBrush = nullptr;
HBRUSH g_appSettingsEditBrush = nullptr;
COLORREF g_appSettingsTextColor = RGB(30, 30, 30);
COLORREF g_appSettingsLabelColor = RGB(80, 80, 80);
COLORREF g_appSettingsEditBackground = RGB(255, 255, 255);

bool g_syncingOpacity = false;
WNDPROC g_hotkeyEditOldProc = nullptr;
UINT g_pendingHotkeyModifiers = 0;
UINT g_pendingHotkeyVk = 0;
int g_originalOpacity = 45;

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

void SetControlFont(HWND control) {
    if (g_appSettingsFont != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_appSettingsFont), TRUE);
    }
}

void EnsureAppSettingsBrushes() {
    const bool dark = !g_app.lightTheme;
    const COLORREF background = dark ? RGB(30, 30, 38) : RGB(246, 246, 246);
    g_appSettingsTextColor = dark ? RGB(238, 238, 246) : RGB(30, 30, 30);
    g_appSettingsLabelColor = dark ? RGB(180, 180, 195) : RGB(80, 80, 80);
    g_appSettingsEditBackground = dark ? RGB(42, 42, 52) : RGB(255, 255, 255);

    if (g_appSettingsBrush != nullptr) {
        DeleteObject(g_appSettingsBrush);
    }
    if (g_appSettingsEditBrush != nullptr) {
        DeleteObject(g_appSettingsEditBrush);
    }
    g_appSettingsBrush = CreateSolidBrush(background);
    g_appSettingsEditBrush = CreateSolidBrush(g_appSettingsEditBackground);
}

void ApplyAppSettingsTheme(HWND hwnd) {
    EnsureAppSettingsBrushes();
    const BOOL darkTitle = g_app.lightTheme ? FALSE : TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkTitle, sizeof(darkTitle));
    const wchar_t* editTheme = g_app.lightTheme ? L"Explorer" : L"DarkMode_Explorer";
    SetWindowTheme(g_opacityEdit, editTheme, nullptr);
    SetWindowTheme(g_hotkeyEdit, editTheme, nullptr);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void LayoutAppSettings(HWND hwnd) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = 20;

    if (g_opacityLabel != nullptr) {
        MoveWindow(g_opacityLabel, margin, 18, 200, 20, TRUE);
    }
    if (g_opacityTrack != nullptr) {
        MoveWindow(g_opacityTrack, margin, 44, std::max(180, width - 200), 40, TRUE);
    }
    if (g_opacityEdit != nullptr) {
        MoveWindow(g_opacityEdit, width - margin - 100, 50, 60, 26, TRUE);
    }
    if (g_opacityPercent != nullptr) {
        MoveWindow(g_opacityPercent, width - margin - 34, 54, 24, 20, TRUE);
    }
    if (g_opacityHint != nullptr) {
        MoveWindow(g_opacityHint, margin, 88, width - margin * 2, 20, TRUE);
    }
    if (g_divider != nullptr) {
        MoveWindow(g_divider, margin, 118, width - margin * 2, 2, TRUE);
    }
    if (g_hotkeyLabel != nullptr) {
        MoveWindow(g_hotkeyLabel, margin, 132, 240, 20, TRUE);
    }
    if (g_hotkeyEdit != nullptr) {
        MoveWindow(g_hotkeyEdit, margin, 156, 240, 28, TRUE);
    }
    if (g_hotkeyClear != nullptr) {
        MoveWindow(g_hotkeyClear, margin + 248, 156, 70, 28, TRUE);
    }
    if (g_hotkeyHint != nullptr) {
        MoveWindow(g_hotkeyHint, margin, 192, width - margin * 2, 20, TRUE);
    }

    const int buttonY = height - 46;
    if (g_customButton != nullptr) {
        MoveWindow(g_customButton, margin, buttonY, 110, 30, TRUE);
    }
    if (g_saveButton != nullptr) {
        MoveWindow(g_saveButton, width - margin - 90, buttonY, 90, 30, TRUE);
    }
    if (g_cancelButton != nullptr) {
        MoveWindow(g_cancelButton, width - margin - 188, buttonY, 90, 30, TRUE);
    }
}

void UpdateOpacityFromEdit() {
    if (g_syncingOpacity) {
        return;
    }
    const std::wstring text = GetControlText(g_opacityEdit);
    if (text.empty()) {
        return;
    }
    int value = _wtoi(text.c_str());
    value = std::clamp(value, 0, 100);
    g_syncingOpacity = true;
    SendMessageW(g_opacityTrack, TBM_SETPOS, TRUE, value);
    g_syncingOpacity = false;
    ApplyHoverTipOpacity(value);
}

void UpdateOpacityFromTrack() {
    if (g_syncingOpacity) {
        return;
    }
    const int value = std::clamp(static_cast<int>(SendMessageW(g_opacityTrack, TBM_GETPOS, 0, 0)),
                                 0, 100);
    g_syncingOpacity = true;
    wchar_t text[16]{};
    swprintf_s(text, L"%d", value);
    SetWindowTextW(g_opacityEdit, text);
    g_syncingOpacity = false;
    ApplyHoverTipOpacity(value);
}

LRESULT CALLBACK HotkeyEditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wParam);
        if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN ||
            vk == VK_RWIN) {
            return 0;
        }
        if (vk == VK_ESCAPE) {
            g_pendingHotkeyModifiers = 0;
            g_pendingHotkeyVk = 0;
            SetWindowTextW(hwnd, L"");
            return 0;
        }

        UINT modifiers = 0;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            modifiers |= MOD_CONTROL;
        }
        if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
            modifiers |= MOD_ALT;
        }
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            modifiers |= MOD_SHIFT;
        }
        if (((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0) {
            modifiers |= MOD_WIN;
        }
        if (modifiers == 0) {
            MessageBeep(MB_ICONWARNING);
            return 0;
        }

        g_pendingHotkeyModifiers = modifiers;
        g_pendingHotkeyVk = vk;
        SetWindowTextW(hwnd, FormatHotkeyText(modifiers, vk).c_str());
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    default:
        break;
    }
    return CallWindowProcW(g_hotkeyEditOldProc, hwnd, message, wParam, lParam);
}

void CancelAppSettings(HWND hwnd) {
    ApplyHoverTipOpacity(g_originalOpacity);
    DestroyWindow(hwnd);
}

void SaveAppSettings(HWND hwnd) {
    const int opacity = std::clamp(static_cast<int>(SendMessageW(g_opacityTrack, TBM_GETPOS, 0, 0)),
                                   0, 100);
    if (g_pendingHotkeyVk != 0 && g_pendingHotkeyModifiers == 0) {
        MessageBoxW(hwnd, L"请设置包含 Ctrl/Alt/Shift/Win 的组合键。", L"设置",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    const std::wstring newHotkey =
        g_pendingHotkeyVk != 0 ? FormatHotkeyText(g_pendingHotkeyModifiers, g_pendingHotkeyVk)
                               : L"";
    const std::wstring oldHotkey = g_app.config.settingsHotkey;
    g_app.config.settingsHotkey = newHotkey;
    g_app.config.hoverOpacityPercent = opacity;
    if (!RegisterAppHotkey()) {
        g_app.config.settingsHotkey = oldHotkey;
        RegisterAppHotkey();
        MessageBoxW(hwnd, L"快捷键注册失败，可能已被其他程序占用，请换一个组合。", L"设置",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    SaveConfig();
    ApplyHoverTipOpacity(opacity);
    DestroyWindow(hwnd);
}

void CreateAppSettingsControls(HWND hwnd) {
    const HINSTANCE instance = g_app.instance;
    g_appSettingsFont =
        CreateFontW(-MulDiv(10, g_app.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

    g_opacityLabel = CreateWindowExW(0, L"STATIC", L"浮窗透明度", WS_CHILD | WS_VISIBLE, 0, 0, 0,
                                     0, hwnd, nullptr, instance, nullptr);
    g_opacityTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_AUTOTICKS,
                                     0, 0, 0, 0, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPACITY_TRACK)),
                                     instance, nullptr);
    g_opacityEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, 0, 0, 0, 0,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPACITY_EDIT)), instance, nullptr);
    g_opacityPercent = CreateWindowExW(0, L"STATIC", L"%", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
                                       nullptr, instance, nullptr);
    g_opacityHint = CreateWindowExW(0, L"STATIC", L"0 = 完全透明，100 = 不透明；拖动滑块或输入数字即时预览",
                                    WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, instance,
                                    nullptr);
    g_divider = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ, 0, 0, 0,
                                0, hwnd, nullptr, instance, nullptr);
    g_hotkeyLabel = CreateWindowExW(0, L"STATIC", L"打开设置的快捷键", WS_CHILD | WS_VISIBLE, 0, 0,
                                    0, 0, hwnd, nullptr, instance, nullptr);
    g_hotkeyEdit = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 0, 0,
        hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOTKEY_EDIT)), instance, nullptr);
    g_hotkeyClear = CreateWindowExW(0, L"BUTTON", L"清除", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
                                    0, 0, 0, hwnd,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HOTKEY_CLEAR)),
                                    instance, nullptr);
    g_hotkeyHint = CreateWindowExW(
        0, L"STATIC", L"点击输入框后按下组合键（需含 Ctrl/Alt/Shift/Win）；留空表示不启用",
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, instance, nullptr);
    g_customButton = CreateWindowExW(
        0, L"BUTTON", L"自定义指标...", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CUSTOM_SETTINGS)), instance, nullptr);
    g_saveButton = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0,
                                   0, 0, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SAVE)), instance,
                                   nullptr);
    g_cancelButton = CreateWindowExW(
        0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL)), instance, nullptr);

    for (HWND control : {g_opacityLabel, g_opacityTrack, g_opacityEdit, g_opacityPercent,
                         g_opacityHint, g_divider, g_hotkeyLabel, g_hotkeyEdit, g_hotkeyClear,
                         g_hotkeyHint, g_customButton, g_saveButton, g_cancelButton}) {
        SetControlFont(control);
    }

    SendMessageW(g_opacityTrack, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_opacityTrack, TBM_SETTICFREQ, 10, 0);
    SendMessageW(g_opacityTrack, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_opacityTrack, TBM_SETPOS, TRUE, g_app.config.hoverOpacityPercent);

    wchar_t opacityText[16]{};
    swprintf_s(opacityText, L"%d", g_app.config.hoverOpacityPercent);
    SetWindowTextW(g_opacityEdit, opacityText);

    UINT modifiers = 0;
    UINT vk = 0;
    if (!g_app.config.settingsHotkey.empty() &&
        ParseHotkeyText(g_app.config.settingsHotkey, modifiers, vk)) {
        g_pendingHotkeyModifiers = modifiers;
        g_pendingHotkeyVk = vk;
        SetWindowTextW(g_hotkeyEdit, FormatHotkeyText(modifiers, vk).c_str());
    } else {
        g_pendingHotkeyModifiers = 0;
        g_pendingHotkeyVk = 0;
        SetWindowTextW(g_hotkeyEdit, L"");
    }

    g_hotkeyEditOldProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_hotkeyEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HotkeyEditProc)));

    LayoutAppSettings(hwnd);
    ApplyAppSettingsTheme(hwnd);
}

LRESULT CALLBACK AppSettingsWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        g_originalOpacity = g_app.config.hoverOpacityPercent;
        CreateAppSettingsControls(hwnd);
        return 0;

    case WM_ERASEBKGND: {
        if (g_appSettingsBrush == nullptr) {
            EnsureAppSettingsBrushes();
        }
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(reinterpret_cast<HDC>(wParam), &client, g_appSettingsBrush);
        return 1;
    }

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_appSettingsLabelColor);
        return reinterpret_cast<LRESULT>(g_appSettingsBrush);

    case WM_CTLCOLOREDIT:
        SetBkColor(reinterpret_cast<HDC>(wParam), g_appSettingsEditBackground);
        SetTextColor(reinterpret_cast<HDC>(wParam), g_appSettingsTextColor);
        return reinterpret_cast<LRESULT>(g_appSettingsEditBrush);

    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_appSettingsBrush);

    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lParam) == g_opacityTrack) {
            UpdateOpacityFromTrack();
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_OPACITY_EDIT:
            if (HIWORD(wParam) == EN_CHANGE) {
                UpdateOpacityFromEdit();
            }
            return 0;
        case IDC_HOTKEY_CLEAR:
            g_pendingHotkeyModifiers = 0;
            g_pendingHotkeyVk = 0;
            SetWindowTextW(g_hotkeyEdit, L"");
            SetFocus(g_hotkeyEdit);
            return 0;
        case IDC_CUSTOM_SETTINGS:
            ShowCustomSettings(hwnd);
            return 0;
        case IDC_SAVE:
            SaveAppSettings(hwnd);
            return 0;
        case IDC_CANCEL:
            CancelAppSettings(hwnd);
            return 0;
        default:
            break;
        }
        break;

    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
        g_app.lastThemeSampleTick = 0;
        RefreshTaskbarWindow(true);
        ApplyAppSettingsTheme(hwnd);
        return 0;

    case WM_SIZE:
        LayoutAppSettings(hwnd);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 520;
        info->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_CLOSE:
        CancelAppSettings(hwnd);
        return 0;

    case WM_DESTROY:
        g_appSettingsWindow = nullptr;
        g_opacityLabel = nullptr;
        g_opacityTrack = nullptr;
        g_opacityEdit = nullptr;
        g_opacityPercent = nullptr;
        g_opacityHint = nullptr;
        g_hotkeyLabel = nullptr;
        g_hotkeyEdit = nullptr;
        g_hotkeyClear = nullptr;
        g_hotkeyHint = nullptr;
        g_customButton = nullptr;
        g_saveButton = nullptr;
        g_cancelButton = nullptr;
        g_divider = nullptr;
        g_hotkeyEditOldProc = nullptr;
        if (g_appSettingsFont != nullptr) {
            DeleteObject(g_appSettingsFont);
            g_appSettingsFont = nullptr;
        }
        if (g_appSettingsBrush != nullptr) {
            DeleteObject(g_appSettingsBrush);
            g_appSettingsBrush = nullptr;
        }
        if (g_appSettingsEditBrush != nullptr) {
            DeleteObject(g_appSettingsEditBrush);
            g_appSettingsEditBrush = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

void ShowAppSettings(HWND owner) {
    if (g_appSettingsWindow != nullptr && IsWindow(g_appSettingsWindow)) {
        ShowWindow(g_appSettingsWindow, IsIconic(g_appSettingsWindow) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(g_appSettingsWindow);
        return;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = AppSettingsWndProc;
    windowClass.hInstance = g_app.instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hIconSm = LoadIconW(g_app.instance, MAKEINTRESOURCEW(101));
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kAppSettingsClass;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        AppendAppLog(L"ShowAppSettings RegisterClassEx failed: " +
                     std::to_wstring(GetLastError()));
        return;
    }

    EnsureAppSettingsBrushes();
    (void)owner;
    g_appSettingsWindow = CreateWindowExW(
        WS_EX_TOOLWINDOW, kAppSettingsClass, L"设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 520, 400, nullptr,
        nullptr, g_app.instance, nullptr);
    if (g_appSettingsWindow == nullptr) {
        AppendAppLog(L"ShowAppSettings CreateWindowEx failed: " +
                     std::to_wstring(GetLastError()));
        return;
    }


    RECT workArea{};
    RECT windowRect{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0) &&
        GetWindowRect(g_appSettingsWindow, &windowRect)) {
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
        const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        SetWindowPos(g_appSettingsWindow, nullptr, x, y, 0, 0,
                     SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(g_appSettingsWindow, SW_SHOW);
    SetWindowPos(g_appSettingsWindow, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    UpdateWindow(g_appSettingsWindow);
    SetForegroundWindow(g_appSettingsWindow);
    SetFocus(g_opacityEdit);
    SendMessageW(g_opacityEdit, EM_SETSEL, 0, -1);
}

void DestroyAppSettings() {
    if (g_appSettingsWindow != nullptr && IsWindow(g_appSettingsWindow)) {
        DestroyWindow(g_appSettingsWindow);
    }
    g_appSettingsWindow = nullptr;
}
