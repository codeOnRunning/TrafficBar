#include "app.h"

#include <algorithm>
#include <cwchar>

namespace {

constexpr const wchar_t* kSessionPrefix = L"TrafficBarNet-";
constexpr const GUID kKernelNetworkProvider = {
    0x7dd42a49, 0x5329, 0x4832, {0x8d, 0xfd, 0x43, 0xd9, 0x79, 0x15, 0x3a, 0x88}};

std::wstring BaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::vector<BYTE> MakeTraceProperties(const std::wstring& name, ULONG mode) {
    const size_t nameBytes = (name.size() + 1) * sizeof(wchar_t);
    std::vector<BYTE> storage(sizeof(EVENT_TRACE_PROPERTIES) + nameBytes, 0);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(storage.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->BufferSize = 64;
    properties->MinimumBuffers = 4;
    properties->MaximumBuffers = 32;
    properties->FlushTimer = 1;
    properties->LogFileMode = mode;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    wcscpy_s(reinterpret_cast<LPWSTR>(storage.data() + properties->LoggerNameOffset),
             name.size() + 1, name.c_str());
    return storage;
}

}  // namespace

ProcessTrafficService::~ProcessTrafficService() {
    Stop();
}

void ProcessTrafficService::Start() {
    if (running_.exchange(true)) {
        return;
    }
    stop_ = false;
    sessionName_ = kSessionPrefix + std::to_wstring(GetCurrentProcessId());
    thread_ = std::thread([this] { Run(); });
}

void ProcessTrafficService::Stop() {
    stop_ = true;
    const TRACEHANDLE trace = traceHandle_.exchange(0);
    if (trace != 0) {
        CloseTrace(trace);
    }

    if (!sessionName_.empty()) {
        std::vector<BYTE> storage = MakeTraceProperties(sessionName_, 0);
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());
        ControlTraceW(0, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void ProcessTrafficService::Run() {
    std::vector<BYTE> storage = MakeTraceProperties(sessionName_, EVENT_TRACE_REAL_TIME_MODE);
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data());

    TRACEHANDLE session = 0;
    if (StartTraceW(&session, sessionName_.c_str(), properties) != ERROR_SUCCESS) {
        running_ = false;
        return;
    }

    const ULONG enableResult =
        EnableTraceEx2(session, &kKernelNetworkProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                       TRACE_LEVEL_INFORMATION, 0, 0, EVENT_TRACE_FLAG_NETWORK_TCPIP, nullptr);
    if (enableResult != ERROR_SUCCESS) {
        ControlTraceW(session, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
        running_ = false;
        return;
    }

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<LPWSTR>(sessionName_.c_str());
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.Context = this;
    log.EventRecordCallback = [](PEVENT_RECORD event) {
        if (event != nullptr && event->UserContext != nullptr) {
            static_cast<ProcessTrafficService*>(event->UserContext)->HandleEvent(event);
        }
    };

    TRACEHANDLE trace = OpenTraceW(&log);
    if (trace == INVALID_PROCESSTRACE_HANDLE) {
        ControlTraceW(session, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
        running_ = false;
        return;
    }

    traceHandle_ = trace;
    ProcessTrace(&trace, 1, nullptr, nullptr);
    traceHandle_ = 0;
    CloseTrace(trace);
    ControlTraceW(session, sessionName_.c_str(), properties, EVENT_TRACE_CONTROL_STOP);
    running_ = false;
}

void ProcessTrafficService::AddBytes(DWORD pid, uint32_t bytes, bool sent) {
    if (bytes == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    ProcessTrafficEntry& entry = entries_[pid];
    entry.pid = pid;
    if (sent) {
        entry.upBytes += bytes;
    } else {
        entry.downBytes += bytes;
    }
}

void ProcessTrafficService::HandleEvent(const EVENT_RECORD* event) {
    if (event == nullptr || event->UserDataLength < sizeof(DWORD) * 2) {
        return;
    }

    bool sent = false;
    switch (event->EventHeader.EventDescriptor.Id) {
    case 10:   // TCPv4 data sent
    case 26:   // TCPv6 data sent
    case 42:   // UDPv4 data sent
    case 58:   // UDPv6 data sent
        sent = true;
        break;
    case 11:   // TCPv4 data received
    case 27:   // TCPv6 data received
    case 43:   // UDPv4 data received
    case 59:   // UDPv6 data received
        sent = false;
        break;
    default:
        return;
    }

    DWORD pid = 0;
    DWORD bytes = 0;
    memcpy(&pid, event->UserData, sizeof(pid));
    memcpy(&bytes, static_cast<const BYTE*>(event->UserData) + sizeof(pid), sizeof(bytes));
    if (pid <= 4 || bytes == 0) {
        return;
    }
    AddBytes(pid, bytes, sent);
}

std::vector<ProcessTrafficEntry> ProcessTrafficService::Snapshot() {
    std::vector<ProcessTrafficEntry> result;
    {
        std::lock_guard lock(mutex_);
        result.reserve(entries_.size());
        for (const auto& [pid, entry] : entries_) {
            (void)pid;
            result.push_back(entry);
        }
    }

    for (auto& entry : result) {
        wchar_t path[1024]{};
        DWORD size = static_cast<DWORD>(_countof(path));
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.pid);
        if (process != nullptr) {
            if (QueryFullProcessImageNameW(process, 0, path, &size)) {
                entry.name = BaseName(path);
            }
            CloseHandle(process);
        }
        if (entry.name.empty()) {
            entry.name = L"PID " + std::to_wstring(entry.pid);
        }
    }

    std::sort(result.begin(), result.end(), [](const ProcessTrafficEntry& a,
                                                const ProcessTrafficEntry& b) {
        return a.upBytes + a.downBytes > b.upBytes + b.downBytes;
    });
    return result;
}
