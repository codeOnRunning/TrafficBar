#pragma once

#include "common.h"

struct TrafficBarPluginMetric {
    wchar_t label[32]{};
    wchar_t value[64]{};
};

using TrafficBarPluginGetMetricsFn = int(__cdecl*)(TrafficBarPluginMetric*, int);

class PluginManager {
public:
    void Load();
    void Unload();
    void Refresh();
    std::vector<std::wstring> Lines();

private:
    struct Plugin {
        HMODULE module = nullptr;
        TrafficBarPluginGetMetricsFn getMetrics = nullptr;
    };

    std::vector<Plugin> plugins_;
    std::vector<std::wstring> lines_;
    ULONGLONG lastRefreshTick_ = 0;
};
