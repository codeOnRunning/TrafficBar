#include "app.h"

#include <cwchar>
#include <cwctype>

namespace {

std::wstring g_cachedConfigPath;

std::wstring TrimText(const std::wstring& text) {
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

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
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

std::wstring QueryExePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return L"";
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DirName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

}  // namespace

std::wstring GetExePath() {
    static const std::wstring path = QueryExePath();
    return path;
}

std::wstring GetExeDir() {
    static const std::wstring dir = DirName(GetExePath());
    return dir;
}

void LoadConfig() {
    g_app.exePath = GetExePath();
    g_app.configPath = GetConfigPath();

    const wchar_t* path = g_app.configPath.c_str();
    g_app.config.showCpu = GetPrivateProfileIntW(L"general", L"show_cpu", 1, path) != 0;
    g_app.config.showMemory = GetPrivateProfileIntW(L"general", L"show_memory", 1, path) != 0;
    g_app.config.showDisk = GetPrivateProfileIntW(L"general", L"show_disk", 0, path) != 0;
    g_app.config.showGpu = GetPrivateProfileIntW(L"general", L"show_gpu", 0, path) != 0;
    g_app.config.intervalMs = GetPrivateProfileIntW(L"general", L"interval_ms", 1000, path);
    g_app.config.intervalMs = std::clamp(g_app.config.intervalMs, 500, 30000);
    g_app.config.fontSize = GetPrivateProfileIntW(L"general", L"font_size", 9, path);
    g_app.config.fontSize = std::clamp(g_app.config.fontSize, 8, 12);
    g_app.config.themeMode =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"general", L"theme_mode", 0, path)), 0, 10);

    wchar_t luidBuffer[64]{};
    GetPrivateProfileStringW(L"general", L"adapter_luid", L"0", luidBuffer,
                             static_cast<DWORD>(_countof(luidBuffer)), path);
    wchar_t* end = nullptr;
    g_app.config.adapterLuid = wcstoull(luidBuffer, &end, 16);

    g_app.config.customEnabled = GetPrivateProfileIntW(L"custom", L"enabled", 0, path) != 0;
    wchar_t primaryBuffer[128]{};
    GetPrivateProfileStringW(L"custom", L"primary", L"", primaryBuffer,
                             static_cast<DWORD>(_countof(primaryBuffer)), path);
    g_app.config.customPrimary = TrimText(primaryBuffer);

    wchar_t itemsBuffer[8192]{};
    GetPrivateProfileStringW(L"custom", L"items", L"", itemsBuffer,
                             static_cast<DWORD>(_countof(itemsBuffer)), path);
    g_app.config.customItems = ParseCustomItems(itemsBuffer);
    g_app.config.customRefreshMs =
        std::clamp(static_cast<int>(GetPrivateProfileIntW(L"custom", L"refresh_ms", 5000, path)),
                   1000, 60000);
    g_app.config.customShowPercent =
        GetPrivateProfileIntW(L"custom", L"show_percent", 0, path) != 0;
    g_app.config.transparentThemeDark =
        GetPrivateProfileIntW(L"general", L"transparent_dark", 0, path) != 0;
    g_app.config.hideStockOffHours =
        GetPrivateProfileIntW(L"custom", L"hide_off_hours", 1, path) != 0;

    if (g_app.config.customItems.empty()) {
        g_app.config.customEnabled = false;
        g_app.config.customPrimary.clear();
    } else {
        bool found = false;
        for (const auto& item : g_app.config.customItems) {
            if (EqualsIgnoreCase(item.alias, g_app.config.customPrimary)) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_app.config.customPrimary = g_app.config.customItems.front().alias;
        }
    }
}

void SaveConfig() {
    if (g_app.configPath.empty()) {
        g_app.configPath = GetConfigPath();
    }
    const wchar_t* path = g_app.configPath.c_str();

    auto append = [](std::wstring& section, const wchar_t* key, const std::wstring& value) {
        section += key;
        section.push_back(L'=');
        section += value;
        section.push_back(L'\0');
    };

    std::wstring general;
    append(general, L"show_cpu", g_app.config.showCpu ? L"1" : L"0");
    append(general, L"show_memory", g_app.config.showMemory ? L"1" : L"0");
    append(general, L"show_disk", g_app.config.showDisk ? L"1" : L"0");
    append(general, L"show_gpu", g_app.config.showGpu ? L"1" : L"0");

    wchar_t interval[32]{};
    swprintf_s(interval, L"%d", g_app.config.intervalMs);
    append(general, L"interval_ms", interval);

    wchar_t font[32]{};
    swprintf_s(font, L"%d", g_app.config.fontSize);
    append(general, L"font_size", font);

    wchar_t theme[32]{};
    swprintf_s(theme, L"%d", g_app.config.themeMode);
    append(general, L"theme_mode", theme);

    append(general, L"transparent_dark", g_app.config.transparentThemeDark ? L"1" : L"0");

    wchar_t luid[32]{};
    swprintf_s(luid, L"%016llX", static_cast<unsigned long long>(g_app.config.adapterLuid));
    append(general, L"adapter_luid", luid);
    general.push_back(L'\0');
    WritePrivateProfileSectionW(L"general", general.c_str(), path);

    std::wstring primary = g_app.config.customPrimary;
    if (!primary.empty()) {
        bool found = false;
        for (const auto& item : g_app.config.customItems) {
            if (EqualsIgnoreCase(item.alias, primary)) {
                found = true;
                break;
            }
        }
        if (!found) {
            primary.clear();
        }
    }
    if (primary.empty() && !g_app.config.customItems.empty()) {
        primary = g_app.config.customItems.front().alias;
    }

    std::wstring custom;
    append(custom, L"enabled",
           g_app.config.customEnabled && !g_app.config.customItems.empty() ? L"1" : L"0");
    append(custom, L"primary", primary);
    append(custom, L"items", FormatCustomItems(g_app.config.customItems));

    wchar_t refresh[32]{};
    swprintf_s(refresh, L"%d", g_app.config.customRefreshMs);
    append(custom, L"refresh_ms", refresh);
    append(custom, L"show_percent", g_app.config.customShowPercent ? L"1" : L"0");
    append(custom, L"hide_off_hours", g_app.config.hideStockOffHours ? L"1" : L"0");
    custom.push_back(L'\0');
    WritePrivateProfileSectionW(L"custom", custom.c_str(), path);
}

std::wstring GetConfigPath() {
    if (!g_cachedConfigPath.empty()) {
        return g_cachedConfigPath;
    }

    const std::wstring local = GetExeDir() + L"\\TrafficBar.ini";
    HANDLE file = CreateFileW(local.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        CloseHandle(file);
        g_cachedConfigPath = local;
        return g_cachedConfigPath;
    }

    wchar_t appData[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appData))) {
        std::wstring dir = std::wstring(appData) + L"\\TrafficBar";
        CreateDirectoryW(dir.c_str(), nullptr);
        g_cachedConfigPath = dir + L"\\TrafficBar.ini";
    } else {
        g_cachedConfigPath = L"TrafficBar.ini";
    }
    return g_cachedConfigPath;
}

std::wstring GetHistoryPath() {
    const std::wstring config = GetConfigPath();
    const size_t slash = config.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"TrafficBar_history.dat";
    }
    return config.substr(0, slash) + L"\\TrafficBar_history.dat";
}

std::wstring NormalizeCustomCode(const std::wstring& raw) {
    std::wstring code = TrimText(raw);
    if (code.empty()) {
        return L"";
    }

    const size_t dot = code.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0 && dot + 1 < code.size()) {
        const std::wstring suffix = code.substr(dot + 1);
        const std::wstring base = code.substr(0, dot);
        if (EqualsIgnoreCase(suffix, L"sh") || EqualsIgnoreCase(suffix, L"sz") ||
            EqualsIgnoreCase(suffix, L"bj") || EqualsIgnoreCase(suffix, L"hk") ||
            EqualsIgnoreCase(suffix, L"us")) {
            code = suffix + base;
        } else {
            code = base;
        }
    }

    auto allDigits = [](const std::wstring& value) {
        return std::all_of(value.begin(), value.end(),
                           [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
    };
    auto lower = [](std::wstring value) {
        for (wchar_t& ch : value) {
            ch = static_cast<wchar_t>(towlower(ch));
        }
        return value;
    };
    auto upper = [](std::wstring value) {
        for (wchar_t& ch : value) {
            ch = static_cast<wchar_t>(towupper(ch));
        }
        return value;
    };

    if (code.size() > 2) {
        const std::wstring prefix = lower(code.substr(0, 2));
        const std::wstring rest = code.substr(2);
        if (prefix == L"sh" || prefix == L"sz" || prefix == L"bj" || prefix == L"hk") {
            if (!rest.empty() && allDigits(rest)) {
                return prefix + rest;
            }
        } else if (prefix == L"us") {
            if (!rest.empty()) {
                return L"us" + upper(rest);
            }
        }
    }

    if (allDigits(code)) {
        if (code.size() == 6) {
            const wchar_t first = code[0];
            if (first == L'6' || first == L'9' || first == L'5') {
                return L"sh" + code;
            }
            if (first == L'4' || first == L'8') {
                return L"bj" + code;
            }
            return L"sz" + code;
        }
        if (code.size() == 5) {
            return L"hk" + code;
        }
        return L"";
    }

    if (std::all_of(code.begin(), code.end(), [](wchar_t ch) { return iswalnum(ch); })) {
        return L"us" + upper(code);
    }
    return L"";
}

std::vector<CustomItem> ParseCustomItems(const std::wstring& text) {
    std::wstring normalized;
    normalized.reserve(text.size());
    for (wchar_t ch : text) {
        if (ch == L'\r' || ch == L'\n' || ch == L',') {
            normalized.push_back(L';');
        } else {
            normalized.push_back(ch);
        }
    }

    std::vector<CustomItem> items;
    size_t start = 0;
    while (start <= normalized.size() && items.size() < 30) {
        const size_t separator = normalized.find(L';', start);
        const std::wstring token =
            TrimText(normalized.substr(start, separator == std::wstring::npos
                                                  ? std::wstring::npos
                                                  : separator - start));
        if (!token.empty() && token[0] != L'#' && token[0] != L';') {
            const size_t equals = token.find(L'=');
            if (equals != std::wstring::npos) {
                std::wstring alias = TrimText(token.substr(0, equals));
                const std::wstring code = NormalizeCustomCode(token.substr(equals + 1));
                if (!alias.empty() && alias.size() <= 12 && !code.empty()) {
                    bool duplicate = false;
                    for (const auto& item : items) {
                        if (EqualsIgnoreCase(item.alias, alias)) {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate) {
                        items.push_back({alias, code});
                    }
                }
            }
        }
        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1;
    }
    return items;
}

std::wstring FormatCustomItems(const std::vector<CustomItem>& items) {
    std::wstring result;
    for (const auto& item : items) {
        if (!result.empty()) {
            result.push_back(L';');
        }
        result += item.alias;
        result.push_back(L'=');
        result += item.code;
    }
    return result;
}

namespace {

std::wstring SchTasksPath() {
    wchar_t systemDir[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir));
    if (length == 0 || length >= ARRAYSIZE(systemDir)) {
        return L"schtasks.exe";
    }
    return std::wstring(systemDir) + L"\\schtasks.exe";
}

bool RunHiddenAndWait(const std::wstring& commandLine, DWORD& exitCode) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    bool ok = false;
    if (wait == WAIT_OBJECT_0) {
        ok = GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

void RemoveRunEntry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kAppName);
        RegCloseKey(key);
    }
}

void SetRunEntry(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (enabled) {
        const std::wstring command = L"\"" + GetExePath() + L"\" --autostart";
        RegSetValueExW(key, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kAppName);
    }
    RegCloseKey(key);
}

std::wstring XmlEscape(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    for (wchar_t ch : text) {
        switch (ch) {
        case L'&':
            result += L"&amp;";
            break;
        case L'<':
            result += L"&lt;";
            break;
        case L'>':
            result += L"&gt;";
            break;
        case L'"':
            result += L"&quot;";
            break;
        case L'\'':
            result += L"&apos;";
            break;
        default:
            result.push_back(ch);
            break;
        }
    }
    return result;
}

bool CreateScheduledTask() {
    wchar_t userName[256]{};
    wchar_t domainName[256]{};
    GetEnvironmentVariableW(L"USERNAME", userName, ARRAYSIZE(userName));
    GetEnvironmentVariableW(L"USERDOMAIN", domainName, ARRAYSIZE(domainName));
    const std::wstring userId =
        std::wstring(domainName) + L"\\" + userName;

    const std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Description>TrafficBar autostart</Description></RegistrationInfo>\r\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><Delay>PT10S</Delay></LogonTrigger></Triggers>\r\n"
        L"  <Principals><Principal id=\"Author\"><UserId>" +
        XmlEscape(userId) +
        L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel>"
        L"</Principal></Principals>\r\n"
        L"  <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate>"
        L"<StartWhenAvailable>true</StartWhenAvailable><RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>7</Priority>"
        L"<RestartOnFailure><Interval>PT1M</Interval><Count>3</Count></RestartOnFailure></Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" +
        XmlEscape(GetExePath()) +
        L"</Command><Arguments>--autostart</Arguments></Exec></Actions>\r\n"
        L"</Task>\r\n";

    wchar_t tempDir[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};
    if (GetTempPathW(ARRAYSIZE(tempDir), tempDir) == 0 ||
        GetTempFileNameW(tempDir, L"TrafficBar", 0, tempFile) == 0) {
        return false;
    }

    HANDLE file = CreateFileW(tempFile, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(tempFile);
        return false;
    }
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const BOOL wroteBom = WriteFile(file, &bom, sizeof(bom), &written, nullptr);
    const BOOL wroteXml =
        WriteFile(file, xml.c_str(), static_cast<DWORD>(xml.size() * sizeof(wchar_t)), &written,
                  nullptr);
    CloseHandle(file);
    if (!wroteBom || !wroteXml) {
        DeleteFileW(tempFile);
        return false;
    }

    const std::wstring arguments = L"/Create /TN \"" + std::wstring(kAppName) + L"\" /XML \"" +
                                   tempFile + L"\" /F";
    DWORD exitCode = 1;
    const bool ok = RunHiddenAndWait(L"\"" + SchTasksPath() + L"\" " + arguments, exitCode);
    DeleteFileW(tempFile);
    return ok;
}

bool RunEntryExists() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[1024]{};
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LSTATUS status =
        RegQueryValueExW(key, kAppName, nullptr, &type, reinterpret_cast<LPBYTE>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

}  // namespace

bool IsTaskAutoStartEnabled() {
    const std::wstring taskFile = L"C:\\Windows\\System32\\Tasks\\" + std::wstring(kAppName);
    return GetFileAttributesW(taskFile.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsAutoStartEnabled() {
    return IsTaskAutoStartEnabled() || RunEntryExists();
}

void SetAutoStart(bool enabled) {
    if (enabled) {
        if (CreateScheduledTask()) {
            RemoveRunEntry();
        } else {
            SetRunEntry(true);
        }
    } else {
        const std::wstring schtasks = SchTasksPath();
        const std::wstring arguments =
            L"/Delete /TN \"" + std::wstring(kAppName) + L"\" /F";
        DWORD exitCode = 1;
        RunHiddenAndWait(L"\"" + schtasks + L"\" " + arguments, exitCode);
        SetRunEntry(false);
    }
}

void EnsureAutoStartUpgrade() {
    // Older builds only had the HKCU Run entry. It cannot reliably launch an
    // elevated application on logon, so migrate it to a Task Scheduler task.
    if (!IsTaskAutoStartEnabled() && RunEntryExists()) {
        SetAutoStart(true);
    }
}

void AppendAppLog(const std::wstring& text) {
    SYSTEMTIME time{};
    GetLocalTime(&time);

    wchar_t line[1024]{};
    swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u  %ls\r\n", time.wYear, time.wMonth,
               time.wDay, time.wHour, time.wMinute, time.wSecond, text.c_str());

    const std::wstring path = GetExeDir() + L"\\TrafficBar.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
    if (bytes > 1) {
        std::string utf8(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}
