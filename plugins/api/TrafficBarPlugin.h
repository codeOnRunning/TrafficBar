#pragma once

#include <windows.h>

struct TrafficBarPluginMetric {
    wchar_t label[32]{};
    wchar_t value[64]{};
};

// Export this function from a plugin DLL placed in TrafficBar\plugins.
// Return the number of valid entries written to metrics.
extern "C" __declspec(dllexport) int __cdecl TrafficBarPluginGetMetrics(
    TrafficBarPluginMetric* metrics, int capacity);
