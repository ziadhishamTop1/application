#pragma once
#include <intrin.h>
#include <immintrin.h>
#include <iostream>
#include <windows.h>
#include <string>
#include <stdexcept>
#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <functional>
#include <vector>
#include <optional>
#include "binary-parsing.hpp"
#include "buffers_generated.h"

#define ALIGNED_TYPE(type, alignment) alignas(alignment) type
using namespace std;

// Define structs 
struct AVXDepthData {
    std::vector<float> bid_prices;
    std::vector<float> bid_qtys;
    std::vector<float> ask_prices;
    std::vector<float> ask_qtys;
    uint64_t last_update_id;

    // AVX storage
    ALIGNED_TYPE(float, 32) avx_bids[8] = { 0 };
    ALIGNED_TYPE(float, 32) avx_asks[8] = { 0 };
    ALIGNED_TYPE(float, 32) avx_bid_qtys[8] = { 0 };
    ALIGNED_TYPE(float, 32) avx_ask_qtys[8] = { 0 };

    void prepareAVX() {
        size_t levels = min<size_t>(8, bid_prices.size());
        for (size_t i = 0; i < levels; ++i) {
            avx_bids[i] = bid_prices[i];
            avx_asks[i] = ask_prices[i];
            avx_bid_qtys[i] = bid_qtys[i];
            avx_ask_qtys[i] = ask_qtys[i];
        }
    }
};

struct AVXTradeData {
    float price;
    float quantity;
    uint64_t trade_id;
    bool is_buyer_maker;

    // AVX storage (for batch processing)
    ALIGNED_TYPE(float, 32) avx_trade_data[8] = { 0 }; // [price, qty, price, qty,...]
};

struct AVXOrderReportData {
    string id;
    string client_order_id;
    string symbol;
    string side;
    string status;
	double executed_qty;
};

struct DepthUpdateEvent {
    std::vector<std::pair<float, float>> bids; // price, quantity
    std::vector<std::pair<float, float>> asks;
    uint64_t last_update_id;
    uint64_t event_time;
};

struct TradeEvent {
    float price;
    float quantity;
    uint64_t trade_id;
    bool is_buyer_maker;
    uint64_t event_time;
};

struct OrderReportEvent {
	string id;
	string client_order_id;
    string symbol;
    string side;
    string status;
	double executed_qty;
};

enum class ParsedMessageType {
    Depth,
    Trade,
    OrderReport,
    Unknown
};

struct AVXMessage {
    ParsedMessageType type;
    AVXDepthData depth;
    AVXTradeData trade;
    AVXOrderReportData report;
};

// Ultimate Shared Ring Buffer Memory
class PersistentSharedBuffer {
public:
    PersistentSharedBuffer(const std::string& name, size_t size, bool create_new)
        : name_(name), size_(size), is_creator_(create_new) {

        h_map_file_ = CreateFileMapping(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(size_ + sizeof(ControlBlock)),
            name_.c_str());

        if (h_map_file_ == NULL) {
            throw std::runtime_error("Creating File Mapping Failed: " + std::to_string(GetLastError()));
        }

        buffer_ = MapViewOfFile(h_map_file_, FILE_MAP_ALL_ACCESS, 0, 0, size_ + sizeof(ControlBlock));

        if (buffer_ == NULL) {
            CloseHandle(h_map_file_);
            throw std::runtime_error("Map View Of File Failed: " + std::to_string(GetLastError()));
        }

        ctrl_ = reinterpret_cast<ControlBlock*>(buffer_);
        data_ = reinterpret_cast<char*>(buffer_) + sizeof(ControlBlock);

        if (is_creator_) {
            InitializeCriticalSection(&ctrl_->cs);
            ctrl_->head = 0;
            ctrl_->tail = 0;
            ctrl_->count = 0;
            ctrl_->shutdown = false;
        }
    }

    ~PersistentSharedBuffer() {
        if (is_creator_) {
            ctrl_->shutdown = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (buffer_) UnmapViewOfFile(buffer_);
        if (h_map_file_) CloseHandle(h_map_file_);
        if (is_creator_) DeleteCriticalSection(&ctrl_->cs);
    }

    bool push(const void* data, size_t data_size) {
        if (data_size > size_) return false;

        EnterCriticalSection(&ctrl_->cs);

        while ((size_ - ctrl_->count) < data_size && !ctrl_->shutdown) {
            LeaveCriticalSection(&ctrl_->cs);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EnterCriticalSection(&ctrl_->cs);
        }

        if (ctrl_->shutdown) {
            LeaveCriticalSection(&ctrl_->cs);
            return false;
        }

        size_t first_chunk = std::min(data_size, size_ - ctrl_->head);
        memcpy(data_ + ctrl_->head, data, first_chunk);

        if (first_chunk < data_size) {
            memcpy(data_, static_cast<const char*>(data) + first_chunk, data_size - first_chunk);
        }

        ctrl_->head = (ctrl_->head + data_size) % size_;
        ctrl_->count += data_size;

        LeaveCriticalSection(&ctrl_->cs);
        return true;
    }

    bool pop(void* output, size_t expected_size) {
        EnterCriticalSection(&ctrl_->cs);

        while (ctrl_->count < expected_size && !ctrl_->shutdown) {
            LeaveCriticalSection(&ctrl_->cs);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            EnterCriticalSection(&ctrl_->cs);
        }

        if (ctrl_->shutdown && ctrl_->count < expected_size) {
            LeaveCriticalSection(&ctrl_->cs);
            return false;
        }

        size_t first_chunk = std::min(expected_size, size_ - ctrl_->tail);
        memcpy(output, data_ + ctrl_->tail, first_chunk);

        if (first_chunk < expected_size) {
            memcpy(static_cast<char*>(output) + first_chunk, data_, expected_size - first_chunk);
        }

        ctrl_->tail = (ctrl_->tail + expected_size) % size_;
        ctrl_->count -= expected_size;

        LeaveCriticalSection(&ctrl_->cs);
        return true;
    }

    bool should_continue() const {
        return !ctrl_->shutdown;
    }

private:
    struct ControlBlock {
        CRITICAL_SECTION cs;
        size_t head;
        size_t tail;
        size_t count;
        bool shutdown;
    };

    std::string name_;
    size_t size_;
    bool is_creator_;
    HANDLE h_map_file_ = NULL;
    void* buffer_ = nullptr;
    ControlBlock* ctrl_ = nullptr;
    char* data_ = nullptr;
};

// ** Producer Functions **
inline void handleIncomingData(const string& json) {
    static PersistentSharedBuffer shm_buf("Local\\BinanceData", 16 * 1024 * 1024, true);

    binanceParserToFlatbuffers parser(json);
    if (!parser.is_valid) return;

    auto [buffer, size] = parser.getBuffer();
    if (!buffer || size == 0) return;

    uint32_t msg_size = static_cast<uint32_t>(size);
    while (!shm_buf.push(&msg_size, sizeof(msg_size))) {
        this_thread::sleep_for(1ms);
    }

    while (!shm_buf.push(buffer, msg_size)) {
        this_thread::sleep_for(1ms);
    }
}

inline void handleReportData(const string& json) {
    static PersistentSharedBuffer shm_buf("Local\\BinanceReports", 1024 * 1024, true);

    binanceParserToFlatbuffers parser(json);
    if (!parser.is_valid) return;

    auto [buffer, size] = parser.getBuffer();
    if (!buffer || size == 0) return;

    uint32_t msg_size = static_cast<uint32_t>(size);
    while (!shm_buf.push(&msg_size, sizeof(msg_size))) {
        this_thread::sleep_for(1ms);
    }

    while (!shm_buf.push(buffer, msg_size)) {
        this_thread::sleep_for(1ms);
    }
}

// ** Consumer Functions **
class AVXProcessor {
public:
    // Utility function to log AVX register values
    static void logAVXRegister(const std::string& label, const __m256& reg) {
        alignas(32) float values[8];
        _mm256_store_ps(values, reg);

        cout << label << ": ";
        for (float value : values) {
            cout << value << " ";
        }
        cout << endl;
    }

    static void processDepth(const AVXDepthData& depth) {
        __m256 bids = _mm256_load_ps(depth.avx_bids);
        __m256 asks = _mm256_load_ps(depth.avx_asks);
        __m256 bid_qtys = _mm256_load_ps(depth.avx_bid_qtys);
        __m256 ask_qtys = _mm256_load_ps(depth.avx_ask_qtys);

        logAVXRegister("Bids", bids);
        logAVXRegister("Asks", asks);
        logAVXRegister("Bid Quantities", bid_qtys);
        logAVXRegister("Ask Quantities", ask_qtys);

        // Example AVX processing
        __m256 mid_price = _mm256_div_ps(_mm256_add_ps(bids, asks), _mm256_set1_ps(2.0f));
        logAVXRegister("Mid Price", mid_price);
    }

    static void processTrade(const AVXTradeData& trade) {
        ALIGNED_TYPE(float, 32) trade_data[8] = {
            trade.price, trade.quantity,
            0, 0, 0, 0, 0, 0 // Padding for AVX
        };
        __m256 trade_vec = _mm256_load_ps(trade_data);
        logAVXRegister("Trade Data", trade_vec);
    }
};

inline AVXMessage processFlatBufferReturns(const uint8_t* data, uint32_t size) {
    AVXMessage out{};
    auto envelope = Binance::fb::GetEnvelope(data);

    if (!envelope) {
        out.type = ParsedMessageType::Unknown;
        return out;
    }

    switch (envelope->type()) {
    case Binance::fb::MessageType_DepthUpdate: {
        out.type = ParsedMessageType::Depth;
        auto update = envelope->payload_as<Binance::fb::DepthUpdate>();
        if (!update) break;

        // Process bids
        if (update->bids()) {
            for (const auto& bid : *update->bids()) {
                if (bid) {
                    out.depth.bid_prices.push_back(static_cast<float>(bid->price()));
                    out.depth.bid_qtys.push_back(static_cast<float>(bid->quantity()));
                }
            }
        }

        // Process asks
        if (update->asks()) {
            for (const auto& ask : *update->asks()) {
                if (ask) {
                    out.depth.ask_prices.push_back(static_cast<float>(ask->price()));
                    out.depth.ask_qtys.push_back(static_cast<float>(ask->quantity()));
                }
            }
        }

        out.depth.last_update_id = update->last_update_id();
        out.depth.prepareAVX();
        break;
    }
    case Binance::fb::MessageType_Trade: {
        out.type = ParsedMessageType::Trade;
        auto trade = envelope->payload_as<Binance::fb::Trade>();
        if (trade) {
            out.trade.price = static_cast<float>(trade->price());
            out.trade.quantity = static_cast<float>(trade->quantity());
            out.trade.trade_id = trade->trade_id();
            out.trade.is_buyer_maker = trade->is_buyer_maker();
        }
        break;
    }
    case Binance::fb::MessageType_orderReport: {
        out.type = ParsedMessageType::OrderReport;
        auto report = envelope->payload_as<Binance::fb::orderReport>();
        if (report) {
            out.report.symbol = report->symbol()->str();
			out.report.id = to_string(report->id());
			out.report.client_order_id = report->client_order_id()->str();
			out.report.side = report->side()->str();
			out.report.status = report->status()->str();
			out.report.executed_qty = report->executed_qty();
        }
        break;
    }
    default:
        out.type = ParsedMessageType::Unknown;
    }

    return out;
}

class MarketDataDispatcher {
public:
    // Callback function types
    using DepthCallback = std::function<void(const DepthUpdateEvent&)>;
    using TradeCallback = std::function<void(const TradeEvent&)>;
    using OrderReportCallback = std::function<void(const OrderReportEvent&)>;

    void setDepthCallback(DepthCallback cb) { depth_callback_ = cb; }
    void setTradeCallback(TradeCallback cb) { trade_callback_ = cb; }
    void setOrderReportCallback(OrderReportCallback cb) { order_report_callback_ = cb; }

    void dispatch(const AVXMessage& msg) {
        switch (msg.type) {
        case ParsedMessageType::Depth:
            if (depth_callback_) {
                DepthUpdateEvent event;
                // Convert bids
                for (size_t i = 0; i < msg.depth.bid_prices.size(); i++) {
                    event.bids.emplace_back(msg.depth.bid_prices[i], msg.depth.bid_qtys[i]);
                }
                // Convert asks
                for (size_t i = 0; i < msg.depth.ask_prices.size(); i++) {
                    event.asks.emplace_back(msg.depth.ask_prices[i], msg.depth.ask_qtys[i]);
                }
                event.last_update_id = msg.depth.last_update_id;
                event.event_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                depth_callback_(event);
            }
            break;

        case ParsedMessageType::Trade:
            if (trade_callback_) {
                TradeEvent event;
                event.price = msg.trade.price;
                event.quantity = msg.trade.quantity;
                event.trade_id = msg.trade.trade_id;
                event.is_buyer_maker = msg.trade.is_buyer_maker;
                event.event_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

                trade_callback_(event);
            }
            break;

        case ParsedMessageType::OrderReport:
            if (order_report_callback_) {
                OrderReportEvent event;
                event.symbol = msg.report.symbol;
				event.id = msg.report.id;
				event.client_order_id = msg.report.client_order_id;
				event.side = msg.report.side;
				event.status = msg.report.status;
				event.executed_qty = msg.report.executed_qty;

                order_report_callback_(event);
            }
            break;

        default:
            break;
        }
    }

private:
    DepthCallback depth_callback_;
    TradeCallback trade_callback_;
    OrderReportCallback order_report_callback_;
};

inline void consumeMessages(MarketDataDispatcher& dispatcher) {
    PersistentSharedBuffer shm_buf("Local\\BinanceData", 16 * 1024 * 1024, false);
    uint32_t msg_size = 0;
    const uint32_t MAX_MSG_SIZE = 10 * 1024 * 1024;
    vector<uint8_t> buffer;

    while (shm_buf.should_continue()) {
        if (!shm_buf.pop(&msg_size, sizeof(msg_size))) {
            this_thread::sleep_for(10ms);
            continue;
        }

        if (msg_size == 0 || msg_size > MAX_MSG_SIZE) {
            continue;
        }

        buffer.resize(msg_size);
        if (shm_buf.pop(buffer.data(), msg_size)) {
            try {
				//cout << "Processing message of size: " << msg_size << endl;
                AVXMessage msg = processFlatBufferReturns(buffer.data(), msg_size);
                dispatcher.dispatch(msg);
            }
            catch (const exception& e) {
                cerr << "Error processing message: " << e.what() << endl;
            }
        }
    }
}

inline void consumeReports(MarketDataDispatcher& dispatcher) {
    PersistentSharedBuffer shm_buf("Local\\BinanceReports", 1024 * 1024, false);
    uint32_t msg_size = 0;
    const uint32_t MAX_MSG_SIZE = 10 * 1024 * 1024;
    vector<uint8_t> buffer;

    while (shm_buf.should_continue()) {
        if (!shm_buf.pop(&msg_size, sizeof(msg_size))) {
            this_thread::sleep_for(10ms);
            continue;
        }

        if (msg_size == 0 || msg_size > MAX_MSG_SIZE) {
            continue;
        }

        buffer.resize(msg_size);
        if (shm_buf.pop(buffer.data(), msg_size)) {
            try {
                AVXMessage msg = processFlatBufferReturns(buffer.data(), msg_size);
                dispatcher.dispatch(msg);
            }
            catch (const exception& e) {
                cerr << "Error processing message: " << e.what() << endl;
            }
        }
    }
}