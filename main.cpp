#include "Algorithm.hpp" // Ensure this is included
#include "persistanceSharedRingBuffer.hpp"
#include "executer.hpp"
#include <iostream>
#include <string>
#include <libwebsockets.h>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic> // atomic
#include <thread>
using namespace std;

mutex cvMutex;
mutex coutMutex;

condition_variable cv;
struct lws* binance_connection = nullptr;
atomic<bool> running(true);
bool depthDataUpdated = false;
bool tradeUpdated = false;

atomic<int> iterations{ 0 };
atomic<int> batch_number{ 0 };

void handleMetricData(Market_metrics_manager& metrics_data, BinanceConnector& connector) {
	while (running) {
		unique_lock<mutex> lock(cvMutex);
		cv.wait(lock, [] { return depthDataUpdated || tradeUpdated; });
		if (depthDataUpdated) {
			const auto& prices = metrics_data.get_prices();
			auto [bids, bidQty, asks, askQty, midPrice, bestMidPrice] = metrics_data.getDepthData();
			cout << "OPEN ORDERS COUNT: " << connector.getOpenOrdersCount() << endl;

			// Check if no open orders exist before creating new ones
			if (connector.getOpenOrdersCount() == 0) {
				string new_order = connector.create_order_pair(
					round_price(prices.bid_price),
					round_price(prices.ask_price)
				);
				connector.processPendingOrders();
				iterations++; // Mark that we've sent a new pair

				cout << "[INFO] Order pair created: " << new_order << endl;
				cout << "BATCH-NO: " << ++batch_number << endl;
				cout << "[Place Bid] at price = " << prices.bid_price << endl;
				cout << "[Place Ask] at price = " << prices.ask_price << endl;
			}
			//cout << "[DEPTH] Best Mid: " << fixed << setprecision(2) << bestMidPrice << endl;
			depthDataUpdated = false;
		}

		if (tradeUpdated) {
			auto [price, qty, isMaker] = metrics_data.getTradeData();
			//cout << "[TRADE] " << price << " x " << qty
				//<< (isMaker ? " (Maker)" : " (Taker)") << endl;
			tradeUpdated = false;
		}
	}
}

int main() {
	MarketDataDispatcher dispatcher;
	Market_metrics_manager metrics_data;
	Market_prices global_prices;
	Market_prices prices{};

	// Initialize the BinanceConnector
	auto& connector = BinanceConnector::getInstance();
	connector.setMetricsManager(metrics_data);
    if (!connector.initialize()) {
		std::cerr << "[Failed] to initialize BinanceConnector." << std::endl;
		return 1;
    }

	cout << "[SUCCSED] BinanceConnector initialized." << endl;
	
	thread consumer_thread([&]() {
		consumeMessages(dispatcher);
	});

	dispatcher.setDepthCallback([&](const DepthUpdateEvent& depth) {
		metrics_data.update_depth_metrics(depth);
		{
			lock_guard<mutex> lock(cvMutex);
			depthDataUpdated = true;
		}
		cv.notify_one();
	});

	dispatcher.setTradeCallback([&](const TradeEvent& trade) {
		metrics_data.update_trade_metrics(trade);
		{
			lock_guard<mutex> lock(cvMutex);
			tradeUpdated = true;
			
		}
		cv.notify_one();
	});

	thread print_thread(handleMetricData, ref(metrics_data), ref(connector));

	// Connect to Binance
	auto start_time = chrono::steady_clock::now();
	bool orders_sent = false;
	
	while (running && connector.is_running()) {
		lws_service(connector.get_context(), 0);
		//BinanceConnector::getInstance().check_filled_orders();
	}
	print_thread.join();
    cout << "\nPress enter to exit...";
    cin.get();
    return 0;
}