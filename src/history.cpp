#include "history.h"

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

namespace {

int DateKey(const SYSTEMTIME& time) {
    return static_cast<int>(time.wYear) * 10000 + static_cast<int>(time.wMonth) * 100 +
           static_cast<int>(time.wDay);
}

int TodayKey() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    return DateKey(time);
}

bool IsValidDate(int year, int month, int day) {
    if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1) {
        return false;
    }

    static const int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = kDaysInMonth[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        maxDay = 29;
    }
    return day <= maxDay;
}

}  // namespace

bool HistoryStore::Load(const std::wstring& path) {
    path_ = path;
    days_.clear();
    dirty_ = false;

    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    std::string content;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        content.append(buffer, read);
    }
    CloseHandle(file);

    std::istringstream input(content);
    std::string line;
    bool firstLine = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (firstLine) {
            firstLine = false;
            if (line.rfind("TrafficBarHistory", 0) == 0) {
                continue;
            }
        }

        int year = 0;
        int month = 0;
        int day = 0;
        unsigned long long up = 0;
        unsigned long long down = 0;
        if (sscanf_s(line.c_str(), "%d-%d-%d %llu %llu", &year, &month, &day, &up, &down) != 5) {
            continue;
        }
        if (!IsValidDate(year, month, day)) {
            continue;
        }
        const int key = year * 10000 + month * 100 + day;
        DayTraffic& record = days_[key];
        record.upBytes += up;
        record.downBytes += down;
    }

    return true;
}

bool HistoryStore::Save() {
    if (path_.empty()) {
        return false;
    }

    std::string content = "TrafficBarHistory 1\n";
    char line[160]{};
    for (const auto& [key, record] : days_) {
        sprintf_s(line, "%04d-%02d-%02d %llu %llu\n", key / 10000, (key / 100) % 100, key % 100,
                  static_cast<unsigned long long>(record.upBytes),
                  static_cast<unsigned long long>(record.downBytes));
        content += line;
    }

    const std::wstring tempPath = path_ + L".tmp";
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written,
                              nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || written != content.size()) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    CopyFileW(path_.c_str(), (path_ + L".bak").c_str(), FALSE);
    if (!MoveFileExW(tempPath.c_str(), path_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tempPath.c_str());
        return false;
    }

    dirty_ = false;
    return true;
}

void HistoryStore::Add(uint64_t upDelta, uint64_t downDelta) {
    if (upDelta == 0 && downDelta == 0) {
        return;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    DayTraffic& record = days_[DateKey(time)];
    record.upBytes += upDelta;
    record.downBytes += downDelta;
    dirty_ = true;
}

void HistoryStore::Clear() {
    days_.clear();
    dirty_ = true;
}

DayTraffic HistoryStore::Today() const {
    const auto it = days_.find(TodayKey());
    if (it == days_.end()) {
        return {};
    }
    return it->second;
}

DayTraffic HistoryStore::ThisMonth() const {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const int year = time.wYear;
    const int month = time.wMonth;

    DayTraffic result;
    for (const auto& [key, record] : days_) {
        if (key / 10000 == year && (key / 100) % 100 == month) {
            result.upBytes += record.upBytes;
            result.downBytes += record.downBytes;
        }
    }
    return result;
}

DayTraffic HistoryStore::ThisYear() const {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    const int year = time.wYear;

    DayTraffic result;
    for (const auto& [key, record] : days_) {
        if (key / 10000 == year) {
            result.upBytes += record.upBytes;
            result.downBytes += record.downBytes;
        }
    }
    return result;
}

DayTraffic HistoryStore::Total() const {
    DayTraffic result;
    for (const auto& [key, record] : days_) {
        result.upBytes += record.upBytes;
        result.downBytes += record.downBytes;
    }
    return result;
}

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* const units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    wchar_t buffer[64]{};
    if (unit == 0) {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (value < 10.0) {
        swprintf_s(buffer, L"%.2f %ls", value, units[unit]);
    } else if (value < 100.0) {
        swprintf_s(buffer, L"%.1f %ls", value, units[unit]);
    } else {
        swprintf_s(buffer, L"%.0f %ls", value, units[unit]);
    }
    return buffer;
}
