#include <windows.h>

struct TrafficBarPluginMetric {
    wchar_t label[32]{};
    wchar_t value[64]{};
};

extern "C" __declspec(dllexport) int __cdecl TrafficBarPluginGetMetrics(
    TrafficBarPluginMetric* metrics, int capacity) {
    if (metrics == nullptr || capacity < 2) {
        return 0;
    }
    wcscpy_s(metrics[0].label, L"插件");
    wcscpy_s(metrics[0].value, L"OK");
    wcscpy_s(metrics[1].label, L"示例");
    wcscpy_s(metrics[1].value, L"42");
    return 2;
}
