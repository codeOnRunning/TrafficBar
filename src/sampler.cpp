#include "app.h"

#include <cwchar>

#include <pdhmsg.h>
#include <wbemidl.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

bool IsPhysicalType(ULONG type) {
    return type == IF_TYPE_ETHERNET_CSMACD || type == IF_TYPE_IEEE80211;
}

std::wstring RowName(const MIB_IF_ROW2& row) {
    if (row.Alias[0] != L'\0') {
        return row.Alias;
    }
    return row.Description;
}

using NvmlDevice = void*;
using NvmlInit = int (*)();
using NvmlShutdown = int (*)();
using NvmlGetHandle = int (*)(unsigned, NvmlDevice*);
using NvmlGetTemperature = int (*)(NvmlDevice, int, unsigned*);

HMODULE g_nvmlModule = nullptr;
NvmlInit g_nvmlInit = nullptr;
NvmlShutdown g_nvmlShutdown = nullptr;
NvmlGetHandle g_nvmlGetHandle = nullptr;
NvmlGetTemperature g_nvmlGetTemperature = nullptr;
NvmlDevice g_nvmlDevice = nullptr;
bool g_nvmlFailed = false;
ULONGLONG g_lastGpuTemperatureTick = 0;

}  // namespace

Sampler::~Sampler() {
    if (gpuQuery_ != nullptr) {
        PdhCloseQuery(gpuQuery_);
        gpuQuery_ = nullptr;
    }
}

void Sampler::Reset() {
    snapshot_ = SampleSnapshot{};
    activeLuid_ = 0;
    networkInitialized_ = false;
    prevInOctets_ = 0;
    prevOutOctets_ = 0;
    prevNetworkTick_ = 0;
    cpuInitialized_ = false;
    prevIdle_ = 0;
    prevKernel_ = 0;
    prevUser_ = 0;
}

void Sampler::SetPreferredAdapter(uint64_t luid) {
    preferredLuid_ = luid;
    Reset();
}

std::vector<AdapterInfo> Sampler::EnumerateAdapters() const {
    std::vector<AdapterInfo> result;
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return result;
    }

    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
            continue;
        }

        AdapterInfo info;
        info.luid = row.InterfaceLuid.Value;
        info.name = RowName(row);
        info.description = row.Description;
        info.up = row.OperStatus == IfOperStatusUp;
        info.physical = IsPhysicalType(row.Type);
        info.hardware = row.InterfaceAndOperStatusFlags.HardwareInterface != 0;

        if (!info.physical && !info.up) {
            continue;
        }
        if (info.name.empty()) {
            info.name = L"未知网卡";
        }
        result.push_back(std::move(info));
    }

    FreeMibTable(table);

    // The IP helper table contains many WFP/QoS/Hyper-V pseudo-interfaces. Keep
    // only real hardware NICs when at least one is present.
    const bool hasHardwarePhysical =
        std::any_of(result.begin(), result.end(), [](const AdapterInfo& item) {
            return item.physical && item.hardware;
        });
    if (hasHardwarePhysical) {
        result.erase(std::remove_if(result.begin(), result.end(), [](const AdapterInfo& item) {
                         return !(item.physical && item.hardware);
                     }),
                     result.end());
    }

    std::sort(result.begin(), result.end(), [](const AdapterInfo& a, const AdapterInfo& b) {
        if (a.physical != b.physical) {
            return a.physical > b.physical;
        }
        if (a.up != b.up) {
            return a.up > b.up;
        }
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return result;
}

void Sampler::Tick() {
    if (g_app.sessionLocked) {
        return;
    }
    snapshot_.upBytesDelta = 0;
    snapshot_.downBytesDelta = 0;
    QueryNetwork();
    QueryCpu();
    if (!g_app.safeMode) {
        QueryCpuTemperature();
    }
    QueryMemory();
    if (!g_app.safeMode) {
        QueryGpu();
        QueryGpuTemperature();
        QueryDiskSmart();
    }
    QueryDisk();
}

void Sampler::QueryNetwork() {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) {
        return;
    }

    const MIB_IF_ROW2* chosen = nullptr;
    uint64_t chosenScore = 0;

    // Manual selection wins when the adapter is still present and up.
    if (preferredLuid_ != 0) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.InterfaceLuid.Value == preferredLuid_ && row.OperStatus == IfOperStatusUp) {
                chosen = &row;
                break;
            }
        }
    }

    // Auto: prefer a real hardware NIC that is up and has the most traffic.
    if (chosen == nullptr) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp || !IsPhysicalType(row.Type) ||
                !row.InterfaceAndOperStatusFlags.HardwareInterface) {
                continue;
            }
            const uint64_t score = row.InOctets + row.OutOctets;
            if (chosen == nullptr || score >= chosenScore) {
                chosen = &row;
                chosenScore = score;
            }
        }
    }

    // Fallback: any physical adapter that is up.
    if (chosen == nullptr) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp || !IsPhysicalType(row.Type)) {
                continue;
            }
            const uint64_t score = row.InOctets + row.OutOctets;
            if (chosen == nullptr || score >= chosenScore) {
                chosen = &row;
                chosenScore = score;
            }
        }
    }

    // Last resort: any up, non-loopback, non-tunnel interface with traffic.
    if (chosen == nullptr) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (row.OperStatus != IfOperStatusUp) {
                continue;
            }
            if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.Type == IF_TYPE_TUNNEL) {
                continue;
            }
            const uint64_t score = row.InOctets + row.OutOctets;
            if (chosen == nullptr || score >= chosenScore) {
                chosen = &row;
                chosenScore = score;
            }
        }
    }

    if (chosen == nullptr) {
        snapshot_.hasNetwork = false;
        snapshot_.upBps = 0.0;
        snapshot_.downBps = 0.0;
        snapshot_.upBytesDelta = 0;
        snapshot_.downBytesDelta = 0;
        FreeMibTable(table);
        return;
    }

    const uint64_t luid = chosen->InterfaceLuid.Value;
    const uint64_t inOctets = chosen->InOctets;
    const uint64_t outOctets = chosen->OutOctets;
    const std::wstring name = RowName(*chosen);
    const ULONGLONG now = GetTickCount64();

    if (!networkInitialized_ || luid != activeLuid_) {
        activeLuid_ = luid;
        snapshot_.adapterName = name;
        snapshot_.hasNetwork = true;
        snapshot_.upBps = 0.0;
        snapshot_.downBps = 0.0;
        snapshot_.upBytesDelta = 0;
        snapshot_.downBytesDelta = 0;
        prevInOctets_ = inOctets;
        prevOutOctets_ = outOctets;
        prevNetworkTick_ = now;
        networkInitialized_ = true;
        FreeMibTable(table);
        return;
    }

    if (now > prevNetworkTick_) {
        const double seconds = static_cast<double>(now - prevNetworkTick_) / 1000.0;
        if (seconds >= 0.05) {
            const uint64_t upDelta =
                inOctets >= prevInOctets_ ? inOctets - prevInOctets_ : 0;
            const uint64_t downDelta =
                outOctets >= prevOutOctets_ ? outOctets - prevOutOctets_ : 0;
            snapshot_.upBytesDelta = upDelta;
            snapshot_.downBytesDelta = downDelta;
            snapshot_.upBps = static_cast<double>(upDelta) / seconds;
            snapshot_.downBps = static_cast<double>(downDelta) / seconds;
        }
    }

    snapshot_.adapterName = name;
    snapshot_.hasNetwork = true;
    prevInOctets_ = inOctets;
    prevOutOctets_ = outOctets;
    prevNetworkTick_ = now;

    FreeMibTable(table);
}

void Sampler::QueryCpu() {
    FILETIME idleTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        return;
    }

    const uint64_t idle = FileTimeToU64(idleTime);
    const uint64_t kernel = FileTimeToU64(kernelTime);
    const uint64_t user = FileTimeToU64(userTime);

    if (!cpuInitialized_) {
        prevIdle_ = idle;
        prevKernel_ = kernel;
        prevUser_ = user;
        cpuInitialized_ = true;
        return;
    }

    const uint64_t idleDelta = idle >= prevIdle_ ? idle - prevIdle_ : 0;
    const uint64_t kernelDelta = kernel >= prevKernel_ ? kernel - prevKernel_ : 0;
    const uint64_t userDelta = user >= prevUser_ ? user - prevUser_ : 0;
    const uint64_t total = kernelDelta + userDelta;

    if (total > 0 && idleDelta <= total) {
        double busy = 100.0 * static_cast<double>(total - idleDelta) / static_cast<double>(total);
        snapshot_.cpuPercent = std::clamp(busy, 0.0, 100.0);
    }

    prevIdle_ = idle;
    prevKernel_ = kernel;
    prevUser_ = user;
}

void Sampler::QueryCpuTemperature() {
    static bool comReady = false;
    static bool disabled = false;
    static ULONGLONG lastQueryTick = 0;

    if (disabled) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (lastQueryTick != 0 && now - lastQueryTick < 10000) {
        return;
    }
    lastQueryTick = now;

    if (!comReady) {
        const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
            disabled = true;
            return;
        }
        comReady = true;
    }

    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&locator)))) {
        disabled = true;
        return;
    }

    BSTR namespaceName = SysAllocString(L"ROOT\\WMI");
    IWbemServices* services = nullptr;
    const HRESULT connectResult =
        locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(namespaceName);
    locator->Release();
    if (FAILED(connectResult) || services == nullptr) {
        if (services != nullptr) {
            services->Release();
        }
        disabled = true;
        return;
    }

    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT CurrentTemperature FROM MSAcpi_ThermalZoneTemperature");
    IEnumWbemClassObject* enumerator = nullptr;
    const HRESULT queryResult = services->ExecQuery(
        language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
        &enumerator);
    SysFreeString(language);
    SysFreeString(query);
    if (FAILED(queryResult) || enumerator == nullptr) {
        if (enumerator != nullptr) {
            enumerator->Release();
        }
        services->Release();
        return;
    }

    double maxCelsius = 0.0;
    bool found = false;
    while (true) {
        IWbemClassObject* object = nullptr;
        ULONG returned = 0;
        const HRESULT nextResult = enumerator->Next(500, 1, &object, &returned);
        if (FAILED(nextResult) || returned == 0 || object == nullptr) {
            if (object != nullptr) {
                object->Release();
            }
            break;
        }

        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(object->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr))) {
            long tenthsKelvin = 0;
            if (V_VT(&value) == VT_I4) {
                tenthsKelvin = V_I4(&value);
            } else if (V_VT(&value) == VT_UI4) {
                tenthsKelvin = static_cast<long>(V_UI4(&value));
            }
            if (tenthsKelvin > 0) {
                const double celsius = static_cast<double>(tenthsKelvin) / 10.0 - 273.15;
                if (celsius > 0.0 && celsius < 150.0) {
                    maxCelsius = found ? std::max(maxCelsius, celsius) : celsius;
                    found = true;
                }
            }
        }
        VariantClear(&value);
        object->Release();
    }

    enumerator->Release();
    services->Release();

    snapshot_.cpuTempAvailable = found;
    snapshot_.cpuTempC = found ? static_cast<unsigned>(std::lround(maxCelsius)) : 0;
}

void Sampler::QueryGpu() {
    if (!gpuInitialized_) {
        if (PdhOpenQueryW(nullptr, 0, &gpuQuery_) != ERROR_SUCCESS) {
            return;
        }

        DWORD pathChars = 0;
        PDH_STATUS status = PdhExpandWildCardPathW(
            nullptr, L"\\GPU Engine(*)\\Utilization Percentage", nullptr, &pathChars, 0);
        if (status != PDH_MORE_DATA || pathChars == 0) {
            PdhCloseQuery(gpuQuery_);
            gpuQuery_ = nullptr;
            return;
        }

        std::vector<wchar_t> paths(pathChars);
        status = PdhExpandWildCardPathW(nullptr, L"\\GPU Engine(*)\\Utilization Percentage",
                                        paths.data(), &pathChars, 0);
        if (status != ERROR_SUCCESS) {
            PdhCloseQuery(gpuQuery_);
            gpuQuery_ = nullptr;
            return;
        }

        for (const wchar_t* path = paths.data(); *path != L'\0'; path += wcslen(path) + 1) {
            const std::wstring expanded = path;
            if (expanded.find(L"engtype_3d") == std::wstring::npos &&
                expanded.find(L"engtype_3D") == std::wstring::npos) {
                continue;
            }
            PDH_HCOUNTER counter = nullptr;
            if (PdhAddCounterW(gpuQuery_, path, 0, &counter) == ERROR_SUCCESS) {
                gpuCounters_.push_back(counter);
            }
        }
        if (gpuCounters_.empty()) {
            PdhCloseQuery(gpuQuery_);
            gpuQuery_ = nullptr;
            return;
        }
        gpuInitialized_ = true;
    }

    if (PdhCollectQueryData(gpuQuery_) != ERROR_SUCCESS) {
        return;
    }

    double best = 0.0;
    bool found = false;
    for (PDH_HCOUNTER counter : gpuCounters_) {
        PDH_FMT_COUNTERVALUE value{};
        const PDH_STATUS status = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &value);
        if (status == ERROR_SUCCESS &&
            (value.CStatus == PDH_CSTATUS_VALID_DATA || value.CStatus == PDH_CSTATUS_NEW_DATA)) {
            best = std::max(best, value.doubleValue);
            found = true;
        }
    }
    snapshot_.gpuAvailable = found;
    snapshot_.gpuPercent = found ? std::clamp(best, 0.0, 100.0) : 0.0;
}

void Sampler::QueryGpuTemperature() {
    if (g_nvmlModule == nullptr && !g_nvmlFailed) {
        g_nvmlModule = LoadLibraryW(L"nvml.dll");
        if (g_nvmlModule != nullptr) {
            g_nvmlInit = reinterpret_cast<NvmlInit>(GetProcAddress(g_nvmlModule, "nvmlInit_v2"));
            g_nvmlShutdown =
                reinterpret_cast<NvmlShutdown>(GetProcAddress(g_nvmlModule, "nvmlShutdown"));
            g_nvmlGetHandle = reinterpret_cast<NvmlGetHandle>(
                GetProcAddress(g_nvmlModule, "nvmlDeviceGetHandleByIndex_v2"));
            g_nvmlGetTemperature = reinterpret_cast<NvmlGetTemperature>(
                GetProcAddress(g_nvmlModule, "nvmlDeviceGetTemperature"));

            if (g_nvmlInit != nullptr && g_nvmlShutdown != nullptr &&
                g_nvmlGetHandle != nullptr && g_nvmlGetTemperature != nullptr &&
                g_nvmlInit() == 0 && g_nvmlGetHandle(0, &g_nvmlDevice) == 0) {
                g_lastGpuTemperatureTick = 0;
            } else {
                if (g_nvmlShutdown != nullptr) {
                    g_nvmlShutdown();
                }
                FreeLibrary(g_nvmlModule);
                g_nvmlModule = nullptr;
                g_nvmlFailed = true;
                return;
            }
        } else {
            g_nvmlFailed = true;
            return;
        }
    }

    if (g_nvmlModule == nullptr || g_nvmlDevice == nullptr) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (g_lastGpuTemperatureTick != 0 && now - g_lastGpuTemperatureTick < 5000) {
        return;
    }
    g_lastGpuTemperatureTick = now;

    unsigned temperature = 0;
    if (g_nvmlGetTemperature(g_nvmlDevice, 0, &temperature) == 0 && temperature > 0 &&
        temperature < 150) {
        snapshot_.gpuTempC = temperature;
        snapshot_.gpuTempAvailable = true;
    } else {
        snapshot_.gpuTempAvailable = false;
    }
}

void Sampler::QueryDiskSmart() {
    static bool comReady = false;
    static bool disabled = false;
    static ULONGLONG lastQueryTick = 0;

    if (disabled) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (lastQueryTick != 0 && now - lastQueryTick < 30000) {
        return;
    }
    lastQueryTick = now;

    if (!comReady) {
        const HRESULT initResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initResult) && initResult != RPC_E_CHANGED_MODE) {
            disabled = true;
            return;
        }
        comReady = true;
    }

    IWbemLocator* locator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&locator)))) {
        disabled = true;
        return;
    }

    BSTR namespaceName = SysAllocString(L"ROOT\\WMI");
    IWbemServices* services = nullptr;
    const HRESULT connectResult =
        locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services);
    SysFreeString(namespaceName);
    locator->Release();
    if (FAILED(connectResult) || services == nullptr) {
        if (services != nullptr) {
            services->Release();
        }
        disabled = true;
        return;
    }

    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

    bool healthFound = false;
    bool healthy = true;
    BSTR language = SysAllocString(L"WQL");
    BSTR healthQuery =
        SysAllocString(L"SELECT PredictFailure FROM MSStorageDriver_FailurePredictStatus");
    IEnumWbemClassObject* healthEnumerator = nullptr;
    if (SUCCEEDED(services->ExecQuery(
            language, healthQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &healthEnumerator)) &&
        healthEnumerator != nullptr) {
        while (true) {
            IWbemClassObject* object = nullptr;
            ULONG returned = 0;
            const HRESULT nextResult = healthEnumerator->Next(500, 1, &object, &returned);
            if (FAILED(nextResult) || returned == 0 || object == nullptr) {
                if (object != nullptr) {
                    object->Release();
                }
                break;
            }
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(object->Get(L"PredictFailure", 0, &value, nullptr, nullptr)) &&
                V_VT(&value) == VT_BOOL) {
                healthFound = true;
                if (V_BOOL(&value) != VARIANT_FALSE) {
                    healthy = false;
                }
            }
            VariantClear(&value);
            object->Release();
        }
        healthEnumerator->Release();
    }
    SysFreeString(healthQuery);

    bool temperatureFound = false;
    unsigned temperature = 0;
    BSTR smartQuery =
        SysAllocString(L"SELECT VendorSpecific FROM MSStorageDriver_ATAPISmartData");
    IEnumWbemClassObject* smartEnumerator = nullptr;
    if (SUCCEEDED(services->ExecQuery(
            language, smartQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
            &smartEnumerator)) &&
        smartEnumerator != nullptr) {
        while (true) {
            IWbemClassObject* object = nullptr;
            ULONG returned = 0;
            const HRESULT nextResult = smartEnumerator->Next(500, 1, &object, &returned);
            if (FAILED(nextResult) || returned == 0 || object == nullptr) {
                if (object != nullptr) {
                    object->Release();
                }
                break;
            }

            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(object->Get(L"VendorSpecific", 0, &value, nullptr, nullptr)) &&
                V_VT(&value) == (VT_ARRAY | VT_UI1) && V_ARRAY(&value) != nullptr) {
                SAFEARRAY* array = V_ARRAY(&value);
                LONG lower = 0;
                LONG upper = -1;
                if (SUCCEEDED(SafeArrayGetLBound(array, 1, &lower)) &&
                    SUCCEEDED(SafeArrayGetUBound(array, 1, &upper)) && upper >= lower) {
                    BYTE* data = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(array, reinterpret_cast<void**>(&data)))) {
                        const LONG count = upper - lower + 1;
                        // VendorSpecific starts with two SMART version bytes, then
                        // 12-byte ATA attribute records. Attribute 194 is temperature.
                        for (LONG offset = 2; offset + 12 <= count; offset += 12) {
                            if (data[offset] == 194) {
                                const unsigned raw0 = data[offset + 5];
                                const unsigned raw1 = data[offset + 6];
                                unsigned candidate = raw0;
                                if (candidate == 0 && raw1 != 0) {
                                    candidate = raw1;
                                }
                                if (candidate > 0 && candidate < 150) {
                                    temperature = candidate;
                                    temperatureFound = true;
                                }
                                break;
                            }
                        }
                        SafeArrayUnaccessData(array);
                    }
                }
            }
            VariantClear(&value);
            object->Release();
            if (temperatureFound) {
                break;
            }
        }
        smartEnumerator->Release();
    }
    SysFreeString(smartQuery);
    SysFreeString(language);
    services->Release();

    snapshot_.diskSmartAvailable = healthFound;
    snapshot_.diskSmartHealthy = healthy;
    snapshot_.diskSmartTempC = temperatureFound ? temperature : 0;
}

void Sampler::QueryDisk() {
    wchar_t windowsDir[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDir, ARRAYSIZE(windowsDir)) == 0) {
        return;
    }

    const wchar_t root[] = {windowsDir[0], L':', L'\\', L'\0'};
    ULARGE_INTEGER freeBytes{};
    ULARGE_INTEGER totalBytes{};
    ULARGE_INTEGER totalFreeBytes{};
    if (!GetDiskFreeSpaceExW(root, &freeBytes, &totalBytes, &totalFreeBytes)) {
        return;
    }

    snapshot_.diskName = std::wstring(1, windowsDir[0]) + L":";
    snapshot_.diskFreeBytes = freeBytes.QuadPart;
    snapshot_.diskTotalBytes = totalBytes.QuadPart;
    if (totalBytes.QuadPart > 0) {
        const uint64_t used = totalBytes.QuadPart > freeBytes.QuadPart
                                  ? totalBytes.QuadPart - freeBytes.QuadPart
                                  : 0;
        snapshot_.diskPercent =
            100.0 * static_cast<double>(used) / static_cast<double>(totalBytes.QuadPart);
    }
}

void Sampler::QueryMemory() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        snapshot_.memPercent = static_cast<double>(status.dwMemoryLoad);
        snapshot_.memTotalBytes = status.ullTotalPhys;
        snapshot_.memUsedBytes = status.ullTotalPhys - status.ullAvailPhys;
    }
}

std::wstring FormatSpeed(double bytesPerSecond) {
    if (!std::isfinite(bytesPerSecond) || bytesPerSecond < 0.0) {
        bytesPerSecond = 0.0;
    }

    static const wchar_t* const units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s", L"TB/s"};
    double value = bytesPerSecond;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }

    wchar_t buffer[64]{};
    if (unit == 0) {
        swprintf_s(buffer, L"%.0f %ls", value, units[unit]);
    } else if (value < 10.0) {
        swprintf_s(buffer, L"%.2f %ls", value, units[unit]);
    } else if (value < 100.0) {
        swprintf_s(buffer, L"%.1f %ls", value, units[unit]);
    } else {
        swprintf_s(buffer, L"%.0f %ls", value, units[unit]);
    }
    return buffer;
}

std::wstring FormatPercent(double percent) {
    if (!std::isfinite(percent)) {
        percent = 0.0;
    }
    percent = std::clamp(percent, 0.0, 100.0);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%.0f%%", percent);
    return buffer;
}
