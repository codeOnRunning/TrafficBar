#include "quote.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace {

constexpr const wchar_t* kQuoteHost = L"qt.gtimg.cn";
constexpr int kMinRefreshMs = 1000;
constexpr int kMaxRefreshMs = 60000;

std::wstring AsciiToWide(const std::string& text) {
    std::wstring result;
    result.reserve(text.size());
    for (const unsigned char ch : text) {
        result.push_back(static_cast<wchar_t>(ch));
    }
    return result;
}

double ParseDouble(const std::string& text) {
    if (text.empty()) {
        return 0.0;
    }
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return 0.0;
    }
    return value;
}

std::string HttpGet(const std::wstring& path) {
    HINTERNET session = WinHttpOpen(L"TrafficBar/0.4", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        return {};
    }
    WinHttpSetTimeouts(session, 3000, 3000, 3000, 5000);

    HINTERNET connect =
        WinHttpConnect(session, kQuoteHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connect == nullptr) {
        WinHttpCloseHandle(session);
        return {};
    }

    HINTERNET request =
        WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request == nullptr) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return {};
    }

    const wchar_t* headers =
        L"Accept: */*\r\n"
        L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)\r\n";
    WinHttpAddRequestHeaders(request, headers, static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

    std::string body;
    if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0,
                           0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                WINHTTP_NO_HEADER_INDEX) &&
            status == HTTP_STATUS_OK) {
            DWORD available = 0;
            while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                std::string chunk(static_cast<size_t>(available), '\0');
                DWORD read = 0;
                if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
                    break;
                }
                chunk.resize(read);
                body += chunk;
            }
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return body;
}

struct ParsedQuote {
    std::wstring code;
    double price = 0.0;
    double prevClose = 0.0;
};

std::vector<ParsedQuote> ParseQuotes(const std::string& body) {
    std::vector<ParsedQuote> result;
    size_t position = 0;
    while (position < body.size()) {
        const size_t varStart = body.find("v_", position);
        if (varStart == std::string::npos) {
            break;
        }
        const size_t equals = body.find('=', varStart);
        if (equals == std::string::npos) {
            break;
        }
        const size_t quoteStart = body.find('"', equals);
        if (quoteStart == std::string::npos) {
            break;
        }
        const size_t quoteEnd = body.find('"', quoteStart + 1);
        if (quoteEnd == std::string::npos) {
            break;
        }

        const std::string var = body.substr(varStart + 2, equals - varStart - 2);
        const std::string record = body.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        position = quoteEnd + 1;

        std::vector<std::string> fields;
        size_t fieldStart = 0;
        while (fieldStart <= record.size()) {
            const size_t separator = record.find('~', fieldStart);
            if (separator == std::string::npos) {
                fields.push_back(record.substr(fieldStart));
                break;
            }
            fields.push_back(record.substr(fieldStart, separator - fieldStart));
            fieldStart = separator + 1;
        }
        if (fields.size() <= 4) {
            continue;
        }

        const double price = ParseDouble(fields[3]);
        const double prevClose = ParseDouble(fields[4]);
        if (price <= 0.0 || prevClose <= 0.0) {
            continue;
        }

        ParsedQuote quote;
        quote.code = AsciiToWide(var);
        quote.price = price;
        quote.prevClose = prevClose;
        result.push_back(std::move(quote));
    }
    return result;
}

SYSTEMTIME AddHours(const SYSTEMTIME& time, int hours) {
    FILETIME fileTime{};
    if (!SystemTimeToFileTime(&time, &fileTime)) {
        return time;
    }
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    value.QuadPart += static_cast<ULONGLONG>(hours) * 3600ULL * 10000000ULL;
    fileTime.dwLowDateTime = value.LowPart;
    fileTime.dwHighDateTime = value.HighPart;
    SYSTEMTIME result{};
    FileTimeToSystemTime(&fileTime, &result);
    return result;
}

bool IsWeekday(const SYSTEMTIME& time) {
    return time.wDayOfWeek >= 1 && time.wDayOfWeek <= 5;
}

int MinutesOfDay(const SYSTEMTIME& time) {
    return static_cast<int>(time.wHour) * 60 + static_cast<int>(time.wMinute);
}

bool InRange(int value, int begin, int end) {
    return value >= begin && value <= end;
}

bool IsAShareOpen(const SYSTEMTIME& utc) {
    const SYSTEMTIME local = AddHours(utc, 8);
    if (!IsWeekday(local)) {
        return false;
    }
    const int minutes = MinutesOfDay(local);
    return InRange(minutes, 9 * 60 + 30, 11 * 60 + 30) ||
           InRange(minutes, 13 * 60, 15 * 60);
}

bool IsAShareAuction(const SYSTEMTIME& utc) {
    const SYSTEMTIME local = AddHours(utc, 8);
    if (!IsWeekday(local)) {
        return false;
    }
    const int minutes = MinutesOfDay(local);
    // Call auction and closing auction windows, plus the short gap around
    // them, so the taskbar can still show quotes while the market is trading.
    return InRange(minutes, 9 * 60 + 15, 9 * 60 + 30) ||
           InRange(minutes, 14 * 60 + 55, 15 * 60 + 5);
}

bool IsHongKongOpen(const SYSTEMTIME& utc) {
    const SYSTEMTIME local = AddHours(utc, 8);
    if (!IsWeekday(local)) {
        return false;
    }
    const int minutes = MinutesOfDay(local);
    return InRange(minutes, 9 * 60 + 30, 12 * 60) ||
           InRange(minutes, 13 * 60, 16 * 60 + 10);
}

bool IsUsOpen(const SYSTEMTIME& utc) {
    // Approximate US Eastern time. The exact DST switch only affects polling
    // frequency, not the quote data itself.
    const bool daylight = utc.wMonth >= 4 && utc.wMonth <= 10;
    const SYSTEMTIME local = AddHours(utc, daylight ? -4 : -5);
    if (!IsWeekday(local)) {
        return false;
    }
    const int minutes = MinutesOfDay(local);
    return InRange(minutes, 9 * 60 + 30, 16 * 60);
}

bool IsAnyMarketOpen(const std::vector<CustomItem>& items) {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    for (const auto& item : items) {
        if (item.code.size() >= 2) {
            const wchar_t first = static_cast<wchar_t>(towlower(item.code[0]));
            const wchar_t second = static_cast<wchar_t>(towlower(item.code[1]));
            if (first == L's' && second == L'h') {
                if (IsAShareOpen(utc)) {
                    return true;
                }
            } else if (first == L's' && second == L'z') {
                if (IsAShareOpen(utc)) {
                    return true;
                }
            } else if (first == L'b' && second == L'j') {
                if (IsAShareOpen(utc)) {
                    return true;
                }
            } else if (first == L'h' && second == L'k') {
                if (IsHongKongOpen(utc)) {
                    return true;
                }
            } else if (first == L'u' && second == L's') {
                if (IsUsOpen(utc)) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool HasAnyAuctionPhase(const std::vector<CustomItem>& items) {
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    for (const auto& item : items) {
        if (item.code.size() >= 2) {
            const wchar_t first = static_cast<wchar_t>(towlower(item.code[0]));
            const wchar_t second = static_cast<wchar_t>(towlower(item.code[1]));
            if ((first == L's' && (second == L'h' || second == L'z')) ||
                (first == L'b' && second == L'j')) {
                if (IsAShareAuction(utc)) {
                    return true;
                }
            }
        }
    }
    return false;
}

}  // namespace

QuoteService::~QuoteService() {
    Stop();
}

void QuoteService::Configure(const std::vector<CustomItem>& items, int refreshMs) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items_ = items;
        refreshMs_ = std::clamp(refreshMs, kMinRefreshMs, kMaxRefreshMs);
        quotes_.clear();
        quotes_.reserve(items_.size());
        for (const auto& item : items_) {
            QuoteSnapshot quote;
            quote.alias = item.alias;
            quote.code = item.code;
            quotes_.push_back(std::move(quote));
        }
        wakeRequested_ = true;
    }
    wake_.notify_all();
}

void QuoteService::Start(HWND notifyWindow) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        notifyWindow_ = notifyWindow;
        if (started_) {
            return;
        }
        stop_ = false;
        started_ = true;
    }
    worker_ = std::thread(&QuoteService::WorkerMain, this);
}

void QuoteService::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stop_ = true;
        wakeRequested_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
    stop_ = false;
}

std::vector<QuoteSnapshot> QuoteService::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return quotes_;
}

void QuoteService::WorkerMain() {
    for (;;) {
        std::vector<CustomItem> items;
        int interval = 60000;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_) {
                break;
            }
            items = items_;
            const bool open = AnyMarketOpen(items);
            if (open) {
                interval = refreshMs_;
            }
            marketOpen_ = open;
            auctionPhase_ = HasAnyAuctionPhase(items);
        }

        if (!items.empty()) {
            FetchOnce();
        }

        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(interval),
                       [this] { return stop_ || wakeRequested_; });
        wakeRequested_ = false;
        if (stop_) {
            break;
        }
    }
}

void QuoteService::FetchOnce() {
    std::vector<CustomItem> items;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        items = items_;
    }
    if (items.empty()) {
        return;
    }

    std::wstring query;
    for (const auto& item : items) {
        if (!query.empty()) {
            query.push_back(L',');
        }
        query += item.code;
    }

    const std::string body = HttpGet(L"/q=" + query);
    if (body.empty()) {
        return;
    }

    const auto parsed = ParseQuotes(body);
    if (parsed.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& item : parsed) {
            for (auto& quote : quotes_) {
                if (_wcsicmp(quote.code.c_str(), item.code.c_str()) == 0) {
                    quote.price = item.price;
                    quote.prevClose = item.prevClose;
                    quote.changePercent = (item.price - item.prevClose) / item.prevClose * 100.0;
                    quote.valid = true;
                    break;
                }
            }
        }
    }

    if (notifyWindow_ != nullptr && IsWindow(notifyWindow_)) {
        PostMessageW(notifyWindow_, WM_QUOTE_UPDATED, 0, 0);
    }
}

bool QuoteService::AnyMarketOpen(const std::vector<CustomItem>& items) const {
    return IsAnyMarketOpen(items);
}

bool QuoteService::IsMarketOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return marketOpen_;
}

bool QuoteService::HasAuctionPhase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return auctionPhase_;
}
