#pragma once

#include "common.h"

#include <evntcons.h>
#include <evntrace.h>

#include <atomic>
#include <map>
#include <mutex>
#include <thread>

struct ProcessTrafficEntry {
    DWORD pid = 0;
    std::wstring name;
    uint64_t upBytes = 0;
    uint64_t downBytes = 0;
};

class ProcessTrafficService {
public:
    ProcessTrafficService() = default;
    ~ProcessTrafficService();

    void Start();
    void Stop();
    bool running() const { return running_; }

    std::vector<ProcessTrafficEntry> Snapshot();

private:
    void Run();
    void HandleEvent(const EVENT_RECORD* event);
    void AddBytes(DWORD pid, uint32_t bytes, bool sent);

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::map<DWORD, ProcessTrafficEntry> entries_;
    std::wstring sessionName_;
    std::atomic<TRACEHANDLE> traceHandle_{0};
};

void ShowProcessTrafficWindow(HWND owner);
void RefreshProcessTrafficWindow();
void DestroyProcessTrafficWindow();
