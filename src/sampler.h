#pragma once

#include "common.h"

#include <map>
#include <pdh.h>

struct AdapterInfo {
    uint64_t luid = 0;
    std::wstring name;
    std::wstring description;
    bool up = false;
    bool physical = false;
    bool hardware = false;
};

struct SampleSnapshot {
    double upBps = 0.0;
    double downBps = 0.0;
    uint64_t upBytesDelta = 0;
    uint64_t downBytesDelta = 0;
    double cpuPercent = 0.0;
    double memPercent = 0.0;
    uint64_t memUsedBytes = 0;
    uint64_t memTotalBytes = 0;
    double gpuPercent = 0.0;
    bool gpuAvailable = false;
    unsigned cpuTempC = 0;
    bool cpuTempAvailable = false;
    unsigned gpuTempC = 0;
    bool gpuTempAvailable = false;
    bool diskSmartAvailable = false;
    bool diskSmartHealthy = false;
    unsigned diskSmartTempC = 0;
    double diskPercent = 0.0;
    uint64_t diskFreeBytes = 0;
    uint64_t diskTotalBytes = 0;
    std::wstring diskName;
    std::wstring adapterName;
    bool hasNetwork = false;
};

class Sampler {
public:
    Sampler() = default;
    ~Sampler();

    void Reset();
    void Tick();

    const SampleSnapshot& snapshot() const { return snapshot_; }
    std::vector<AdapterInfo> EnumerateAdapters() const;

    void SetPreferredAdapter(uint64_t luid);
    uint64_t activeAdapterLuid() const { return activeLuid_; }

private:
    void QueryNetwork();
    void QueryCpu();
    void QueryCpuTemperature();
    void QueryMemory();
    void QueryGpu();
    void QueryGpuTemperature();
    void QueryDiskSmart();
    void QueryDisk();

    SampleSnapshot snapshot_;

    uint64_t preferredLuid_ = 0;
    uint64_t activeLuid_ = 0;
    bool networkInitialized_ = false;
    uint64_t prevInOctets_ = 0;
    uint64_t prevOutOctets_ = 0;
    uint64_t prevNetworkTick_ = 0;

    bool cpuInitialized_ = false;
    uint64_t prevIdle_ = 0;
    uint64_t prevKernel_ = 0;
    uint64_t prevUser_ = 0;

    PDH_HQUERY gpuQuery_ = nullptr;
    std::vector<PDH_HCOUNTER> gpuCounters_;
    bool gpuInitialized_ = false;
};
