#pragma once
#include <iostream>
#include <algorithm>
#include <mutex>
#include <cstring>
#include <string>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <immintrin.h>
#include "persistanceSharedRingBuffer.hpp"

#ifdef _WIN32
#define ALIGNED_TYPE(T, N) __declspec(align(N)) T
#else 
#define ALIGNED_TYPE(T, N) T __attribute__((aligned(N)))
#endif

using namespace std;

struct DepthMetrics {
    float order_book_imbalance;
    float depth_imbalance;
    float book_flow_imbalance;
};
struct TradeMetrics {
    float order_flow_imbalance;
    float volatility;
};
struct Market_prices {
	double bid_price;
	double ask_price;
    double bis_close_price;
    double ask_close_price;
};

//helper math functions
inline float horizontal_sum_fast(__m256 x) {
    __m128 hi = _mm256_extractf128_ps(x, 1);  // Extract high 128 bits
    __m128 lo = _mm256_castps256_ps128(x);    // Get low 128 bits
    __m128 sum128 = _mm_add_ps(lo, hi);       // Add lo + hi (4 + 4 floats)

    __m128 shuf = _mm_movehdup_ps(sum128);    // Duplicate odd-index elements
    __m128 sums = _mm_add_ps(sum128, shuf);   // Partial sums

    shuf = _mm_movehl_ps(shuf, sums);         // Move high half to low
    sums = _mm_add_ss(sums, shuf);            // Final horizontal sum

    return _mm_cvtss_f32(sums);               // Extract scalar
}

class VolatilityEstimator {
private:
    mutex update_mutex;
    atomic<double> prev_price{ 0.0 };
    atomic<double> variance{ 0.01 };  // Initialize with 10% annualized vol
    double lambda;
    const double min_log_ret = 1e-8;

public:
    // Constructor to initialize lambda
    VolatilityEstimator(double lambda_value) : lambda(lambda_value) {}

    float update(float current_price_f) {
        double current_price = static_cast<double>(current_price_f);
        if (current_price <= 0.0) return 0.0f;

        {
            lock_guard<mutex> lock(update_mutex);
            double prev = prev_price.load(memory_order_relaxed);

            if (prev > 0.0) {
                double log_ret = log(current_price / prev);

                // Handle micro-changes
                if (fabs(log_ret) < min_log_ret) {
                    log_ret = copysign(min_log_ret, log_ret);
                }

                double new_var = lambda * variance.load() + (1 - lambda) * log_ret * log_ret;
                variance.store(new_var);
            }

            prev_price.store(current_price);
        }

        return static_cast<float>(sqrt(variance.load()));
    }
};

inline DepthMetrics depthMicroStructure(__m256 bidQty, __m256 askQty) {
    // perform OBI depth
	DepthMetrics metrics;
    float bidSum = horizontal_sum_fast(bidQty);
    float askSum = horizontal_sum_fast(askQty);
    metrics.order_book_imbalance = (bidSum - askSum) / (bidSum + askSum);
    metrics.book_flow_imbalance = bidSum;
    metrics.depth_imbalance = bidSum - askSum;
    //cout << "[OBI] Order Book Imbalance: " << order_book_imbalance << endl;
    return metrics;
}

inline TradeMetrics tradeMicroStructure(float tradePrice, float tradeQty, bool is_buyer_maker, VolatilityEstimator& vol_estimator) {
    static int buyer_count = 0, seller_count = 0;
    static float buyer_taker_volume = 0, seller_taker_volume = 0;

    static chrono::steady_clock::time_point startTime = chrono::steady_clock::now();
    auto now = chrono::steady_clock::now();
    auto elapsed = chrono::duration_cast<chrono::milliseconds>(now - startTime).count();
    if (elapsed >= 500) {
        //cout << "[TRADE] Buyer Count: " << buyer_count << endl;
        //cout << "[TRADE] Seller Count: " << seller_count << endl;
        //cout << "[TRADE] Buyer Taker Volume: " << buyer_taker_volume << endl;
        //cout << "[TRADE] Seller Taker Volume: " << seller_taker_volume << endl;
        startTime = now; // Reset the timer
    }
    if (is_buyer_maker) {
        seller_count++;
        seller_taker_volume += tradeQty;
    }
    else {
        buyer_count++;
        buyer_taker_volume += tradeQty;
    }

    TradeMetrics metrics;
    float denom = buyer_taker_volume + seller_taker_volume;
    metrics.order_flow_imbalance = denom != 0.0f ? (buyer_taker_volume - seller_taker_volume) / denom : 0.0f;
    metrics.volatility = vol_estimator.update(tradePrice);
	//cout << "[DEBUG] volatility: " << metrics.volatility << endl;
    return metrics;
}

class Market_metrics_manager {
public:
    Market_metrics_manager() :
        bids(_mm256_setzero_ps()),
        asks(_mm256_setzero_ps()),
        bidQty(_mm256_setzero_ps()),
        askQty(_mm256_setzero_ps()),
        midPrice(_mm256_setzero_ps()),
        bestBid(0.0f),
        bestAsk(0.0f),
        bestMidPrice(0.0f),
        tradePrice(0.0f),
        tradeQty(0.0f),
        is_buyer_maker(false),
        vol_estimator(0.94f),
        prices{} { }

    DepthMetrics update_depth_metrics(const DepthUpdateEvent& depth) {
        ALIGNED_TYPE(float, 32) avx_bids[8] = { 0 };
        ALIGNED_TYPE(float, 32) avx_asks[8] = { 0 };
        ALIGNED_TYPE(float, 32) avx_bids_qtys[8] = { 0 };
        ALIGNED_TYPE(float, 32) avx_asks_qtys[8] = { 0 };

        size_t levels = min<size_t>(8, depth.bids.size());
        for (size_t i = 0; i < levels; i++) {
            // Bids
            avx_bids[i] = depth.bids[i].first;
            avx_bids_qtys[i] = depth.bids[i].second;
            // Asks
            avx_asks[i] = depth.asks[i].first;
            avx_asks_qtys[i] = depth.asks[i].second;
        }

        bestBid = avx_bids[0];
        bestAsk = avx_asks[0];
        bids = _mm256_load_ps(avx_bids);
        asks = _mm256_load_ps(avx_asks);
        bidQty = _mm256_load_ps(avx_bids_qtys);
        askQty = _mm256_load_ps(avx_asks_qtys);

        double spread = bestAsk - bestBid;
        midPrice = _mm256_div_ps(_mm256_add_ps(bids, asks), _mm256_set1_ps(2.0f));
        bestMidPrice = (bestBid + bestAsk) / 2.0f;

        d_metrics = depthMicroStructure(bidQty, askQty);
        prices = process_pricing(d_metrics, t_metrics);

        //cout << "[DEPTH] Spread: " << spread << endl;
        return d_metrics;
    }

    TradeMetrics update_trade_metrics(const TradeEvent& trade) {
		tradePrice = trade.price;
		tradeQty = trade.quantity;
        is_buyer_maker = trade.is_buyer_maker;
        t_metrics = tradeMicroStructure(tradePrice, tradeQty, is_buyer_maker, vol_estimator);
        prices = process_pricing(d_metrics, t_metrics);
        return t_metrics;
    }

    Market_prices process_pricing(const DepthMetrics& d_metric, const TradeMetrics& t_metric) {
		Market_prices prices;

        prices.bid_price = bestMidPrice - d_metric.order_book_imbalance + t_metric.volatility - t_metric.order_flow_imbalance;
		prices.ask_price = bestMidPrice + d_metric.order_book_imbalance + t_metric.volatility + t_metric.order_flow_imbalance;
		prices.bis_close_price = bestAsk;
		prices.ask_close_price = bestBid;

		return prices;
    }

    tuple<__m256, float, __m256, float, __m256, float> getDepthData() const {
        lock_guard<mutex> lock(dataMutex_);
        return { bids, _mm256_cvtss_f32(bidQty), asks, _mm256_cvtss_f32(askQty), midPrice, bestMidPrice };
    }

    tuple<float, float, bool> getTradeData() const {
        lock_guard<mutex> lock(dataMutex_);
        return { tradePrice, tradeQty, is_buyer_maker };
    }

    Market_prices get_prices() const {
        lock_guard<mutex> lock(dataMutex_);
        return prices;
    }
private:
    mutable mutex dataMutex_;
    __m256 bids,
        asks,
        bidQty,
        askQty,
        midPrice;

    float tradePrice,
        tradeQty,
        bestMidPrice,
        bestBid,
        bestAsk;

    DepthMetrics d_metrics{};
    TradeMetrics t_metrics{};
    Market_prices prices;
    bool is_buyer_maker;
    VolatilityEstimator vol_estimator;
};

inline string round_price(double price) {
    double factor = pow(10.0, 1);
    double rounded_price = round(price * factor) / factor;
    return to_string(rounded_price);
}
