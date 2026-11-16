#pragma once

#include "common.h"

#include <map>

struct DayTraffic {
    uint64_t upBytes = 0;
    uint64_t downBytes = 0;

    uint64_t Total() const { return upBytes + downBytes; }
};

class HistoryStore {
public:
    bool Load(const std::wstring& path);
    bool Save();
    void Add(uint64_t upDelta, uint64_t downDelta);
    void Clear();

    const std::map<int, DayTraffic>& days() const { return days_; }
    DayTraffic Today() const;
    DayTraffic ThisMonth() const;
    DayTraffic ThisYear() const;
    DayTraffic Total() const;

    bool dirty() const { return dirty_; }
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
    std::map<int, DayTraffic> days_;
    bool dirty_ = false;
};

std::wstring FormatBytes(uint64_t bytes);
