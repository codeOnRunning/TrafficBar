#pragma once

#include "common.h"

#include <condition_variable>
#include <mutex>
#include <thread>

struct QuoteSnapshot {
    std::wstring alias;
    std::wstring code;
    double price = 0.0;
    double prevClose = 0.0;
    double changePercent = 0.0;
    bool valid = false;
};

class QuoteService {
public:
    QuoteService() = default;
    ~QuoteService();

    QuoteService(const QuoteService&) = delete;
    QuoteService& operator=(const QuoteService&) = delete;

    void Configure(const std::vector<CustomItem>& items, int refreshMs);
    void Start(HWND notifyWindow);
    void Stop();

    std::vector<QuoteSnapshot> Snapshot() const;
    bool IsMarketOpen() const;
    bool HasAuctionPhase() const;

private:
    void WorkerMain();
    void FetchOnce();
    bool AnyMarketOpen(const std::vector<CustomItem>& items) const;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::vector<CustomItem> items_;
    std::vector<QuoteSnapshot> quotes_;
    int refreshMs_ = 5000;
    HWND notifyWindow_ = nullptr;
    bool started_ = false;
    bool stop_ = false;
    bool wakeRequested_ = false;
    bool marketOpen_ = false;
    bool auctionPhase_ = false;
};
