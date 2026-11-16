#include "app.h"

#include "plugin.h"

#include <algorithm>

void PluginManager::Load() {
    Unload();

    const std::wstring directory = GetExeDir() + L"\\plugins";
    WIN32_FIND_DATAW findData{};
    HANDLE find = FindFirstFileW((directory + L"\\*.dll").c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring path = directory + L"\\" + findData.cFileName;
        HMODULE module = LoadLibraryW(path.c_str());
        if (module == nullptr) {
            continue;
        }
        auto getMetrics = reinterpret_cast<TrafficBarPluginGetMetricsFn>(
            GetProcAddress(module, "TrafficBarPluginGetMetrics"));
        if (getMetrics == nullptr) {
            FreeLibrary(module);
            continue;
        }
        plugins_.push_back({module, getMetrics});
    } while (FindNextFileW(find, &findData));
    FindClose(find);
}

void PluginManager::Unload() {
    for (const auto& plugin : plugins_) {
        if (plugin.module != nullptr) {
            FreeLibrary(plugin.module);
        }
    }
    plugins_.clear();
    lines_.clear();
    lastRefreshTick_ = 0;
}

void PluginManager::Refresh() {
    const ULONGLONG now = GetTickCount64();
    if (lastRefreshTick_ != 0 && now - lastRefreshTick_ < 2000) {
        return;
    }
    lastRefreshTick_ = now;
    lines_.clear();

    std::vector<TrafficBarPluginMetric> metrics(4);
    for (const auto& plugin : plugins_) {
        if (plugin.getMetrics == nullptr) {
            continue;
        }
        const int count = std::clamp(plugin.getMetrics(metrics.data(), 4), 0, 4);
        for (int i = 0; i < count && lines_.size() < 4; ++i) {
            if (metrics[i].label[0] == L'\0' || metrics[i].value[0] == L'\0') {
                continue;
            }
            metrics[i].label[ARRAYSIZE(metrics[i].label) - 1] = L'\0';
            metrics[i].value[ARRAYSIZE(metrics[i].value) - 1] = L'\0';
            lines_.push_back(std::wstring(metrics[i].label) + L" " + metrics[i].value);
        }
    }
}

std::vector<std::wstring> PluginManager::Lines() {
    Refresh();
    return lines_;
}
