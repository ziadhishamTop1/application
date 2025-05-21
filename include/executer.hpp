#pragma once
#define WIN32_LEAN_AND_MEAN
#include "Algorithm.hpp"
#include <string> 
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <curl/curl.h>
#include <iostream>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>
#include <unordered_map>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>
#include <simdjson.h>
#include <deque>
#include <libwebsockets.h> // Add this include to resolve LWS_PRE

using namespace std;

extern atomic<int> iterations;
extern atomic<int> batch_number;

enum class EndpointType {
    TESTNET,
    USER_DATA,
    MARKET_DATA,
    CLIENT_WS
};

struct ConnectionInfo {
    lws* wsi;
    EndpointType type;
    string path;
};

enum class OrderSide { BUY, SELL };

enum class OrderStatus { 
    NEW,
    PARTIALLY_FILLED,
    FILLED,
    CANCELED,
    EXPIRED,
    EXPIRED_IN_MATCH
};

enum class OrderExecutionType {
    NEW,
    CANCELED,
    CALCULATED,
    EXPIRED,
    TRADE,
    AMENDMENT
};

enum class OrderType { LIMIT,
    MARKET,
    STOP_MARKET,
    STOP_LIMIT,
    TAKE_PROFIT_MARKET,
    TAKE_PROFIT_LIMIT 
};

struct Order {
    string parent_id;
    string client_id;
    string order_id;
    string ID;
    string symbol;
    double price;
    double quantity;
    OrderSide side;
    OrderStatus status;
    double executedQty = 0.0;
    double cummulativeQuoteQty = 0.0;
    string payload;
    bool needs_sending;
    bool needs_closing;
    bool cancelled;
    chrono::system_clock::time_point created;
    vector<pair<double, double>> fills;
};

// class BinanceAPI for RESTFULL requests 
class BinanceAPI {
public:
    BinanceAPI(const std::string& api_key) : api_key_(api_key) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data_) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }

    ~BinanceAPI() {
        curl_global_cleanup();
        WSACleanup();
    }

    std::string get_listen_key() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        std::string response;
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + api_key_).c_str());
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, "https://testnet.binancefuture.com/fapi/v1/listenKey");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

        // For production, enable SSL verification
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

        CURLcode res = curl_easy_perform(curl);

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            throw std::runtime_error("Request failed: " + std::string(curl_easy_strerror(res)));
        }

        if (http_code != 200) {
            throw std::runtime_error("API returned HTTP " + std::to_string(http_code) + ": " + response);
        }

        return extract_listen_key(response);
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* output) {
        output->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

private:

    std::string extract_listen_key(const std::string& response) {
        const std::string key = "\"listenKey\":\"";
        size_t start = response.find(key);
        if (start == std::string::npos) {
            throw std::runtime_error("ListenKey not found in response: " + response);
        }
        start += key.length();
        size_t end = response.find('"', start);
        if (end == std::string::npos) {
            throw std::runtime_error("Malformed listenKey in response");
        }
        return response.substr(start, end - start);
    }

    std::string api_key_;
    WSADATA wsa_data_;
};

// class BinanceConnector for WebSocket connection
class BinanceConnector {
public:
    Market_metrics_manager metrics_data;
    Market_prices prices{0, 0, 0, 0};

    // Singleton instance of BinanceConnector
    static BinanceConnector& getInstance() {
        static BinanceConnector instance;
        return instance;
    }

    // Initialize the WebSocket connection
    bool initialize();
    static struct lws_protocols protocols[];
    struct lws_context* get_context() const { return context; }
    struct lws* get_connection();

    // ListenKey Functions
    string listenKey;
    chrono::system_clock::time_point lastKeyRenewal;
    bool keyExpired() const {
        return std::chrono::system_clock::now() > lastKeyRenewal + std::chrono::minutes(30);
    }

    void broadcastToAllClients(const std::string& message);
    void broadcastToTestnetClients(const std::string& message);
    void broadcastSystemStatus();

    void check_filled_orders();

    string create_order_pair(const string& bidExe, const string& askExe);

    int handleCallback(lws* wsi, lws_callback_reasons reason, void* in, size_t len) {
        ConnectionInfo* conn = getConnection(wsi);

        if (!conn) {
            // 👇 Handle new client connections (if not a Binance connection)
            if (reason == LWS_CALLBACK_ESTABLISHED) {
                std::lock_guard<std::mutex> lock(clients_mutex);
                client_connections.push_back(wsi);
                cout << "New client connected!" << endl;
                return 0;
            }
            return 0;
        }

        switch (conn->type) {
        case EndpointType::TESTNET:
            return handleTestnet(wsi, reason, in, len, *conn);
        case EndpointType::USER_DATA:
            return handleUserData(wsi, reason, in, len, *conn);
        case EndpointType::MARKET_DATA:
            return handleMarketData(wsi, reason, in, len, *conn);
        default:
            return 0;
        }
    }

    // Returns a const reference to the open_orders map
    void queue_order(const string& order_id, const string& payload) {
        lock_guard<mutex> lock(queue_mutex);
        order_queue.emplace_back(order_id, payload);
        write_pending = true;
        queue_cv.notify_one();
        cout << "[QUEUED] Order " << order_id << " added to queue. Queue size: " << order_queue.size() << endl;
    }

    // Function returns open orders count
    int getOpenOrdersCount() const {
        lock_guard<mutex> lock(const_cast<mutex&>(connection_mutex));
        return static_cast<int>(open_orders.size());
    }

    void request_write_callback() {
        if (binance_connection) {
            lws_callback_on_writable(binance_connection);
        }
    }

    void setMetricsManager(Market_metrics_manager& manager) {
        metrics_manager = &manager;
    }

    // Shutdown Flages
    void shutdown();
    bool is_running() const { return running.load(); };

    // Delete copy constructor and assignment operator
    BinanceConnector(const BinanceConnector&) = delete; // Delete copy constructor
    void operator=(const BinanceConnector&) = delete; // Delete assignment operator

    // The callback function for the WebSocket connection
    static int callback_binance(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

    void processPendingOrders() {
        string timestamp = get_current_binance_timestamp();

        // Execute new orders
        {
            vector<string> pending_ids;
            {
                lock_guard<mutex> lock(connection_mutex);
                for (const auto& kv : pending_orders) {
                    if (kv.second.needs_sending) {
                        pending_ids.push_back(kv.first);
                    }
                }
            }

            for (const auto& id : pending_ids) {
                lock_guard<mutex> conn_lock(connection_mutex);
                auto it = pending_orders.find(id);
                if (it == pending_orders.end() || !it->second.needs_sending) continue;

                Order& order = it->second;
                string side = (order.side == OrderSide::BUY) ? "BUY" : "SELL";
                string new_payload = prepare_order_payload("place",
                    to_string(order.price), side, order.client_id, timestamp);

                queue_order(id, new_payload);
                open_orders[id] = order; // Move to open orders
                pending_orders.erase(it);
            }
        }
        if (binance_connection) {
            lws_callback_on_writable(binance_connection); // trigger send
        }
        
    }

    void processClosingOrder(Market_metrics_manager& metrics_manager){
        auto prices = metrics_manager.get_prices();
        string timestamp = get_current_binance_timestamp();
		cout << "[DEBUG] Processing closing orders" << endl;
        vector<string> closing_ids;
        {
            //lock_guard<mutex> lock(connection_mutex);  // ADD THIS
            for (const auto& kv : orders_to_close) {
                if (kv.second.needs_closing) {
                    closing_ids.push_back(kv.first);
                }
            }
        }

        for (const auto& id : closing_ids) {
            lock_guard<mutex> conn_lock(connection_mutex);
            auto it = orders_to_close.find(id);
            if (it == orders_to_close.end() || !it->second.needs_closing) continue;

            Order& order = it->second;
			double price_ = (order.side == OrderSide::BUY) ? prices.ask_price : prices.bid_price;
            string side = (order.side == OrderSide::BUY) ? "BUY" : "SELL";

            // Use actual order quantity instead of hardcoded "0.01"
            string new_payload = close_order_payload(
                round_price(price_),
                side,
                order.client_id,  // Distinct client ID
                timestamp);

			cout << "[DEBUG] Closing order payload: " << new_payload << endl;

            queue_order(id, new_payload);
        }
        if (binance_connection) {
            lws_callback_on_writable(binance_connection); // trigger send
        }
        else {
            cerr << "[ERROR] binance_connection is NULL" << endl;
        }
    }

    void processModifyingOrders(Market_metrics_manager& metrics_manager) {
        auto prices = metrics_manager.get_prices();
        string timestamp = get_current_binance_timestamp();
        cout << "[DEBUG] Processing modifying orders" << endl;
        vector<string> modifying_ids;
        {
            for (const auto& kv : orders_to_modify) {
                modifying_ids.push_back(kv.first);
            }
        }

        for (const auto& id : modifying_ids) {
            lock_guard<mutex> conn_lock(connection_mutex);
            auto it = orders_to_modify.find(id);
            if (it == orders_to_modify.end()) continue;

            Order& order = it->second;
            double price_ = (order.side == OrderSide::BUY) ? prices.bid_price : prices.ask_close_price;
            string side = (order.side == OrderSide::BUY) ? "BUY" : "SELL";

            string new_payload = modify_order_payload(
            round_price(price_),
                side,
                order.client_id,
                timestamp);

            cout << "[DEBUG] Modifying order: (" << order.client_id << ") with ID: (" << order.ID << ")" << endl;
            cout << "[MODIFYING] payload:" << new_payload << endl;

            queue_order(id, new_payload);
            orders_to_modify.erase(it);
        }
        if (binance_connection) {
            lws_callback_on_writable(binance_connection);
        }
        else {
            cerr << "[ERROR] binance_connection is NULL" << endl;
        }
    }

private:
    BinanceConnector();
    ~BinanceConnector();

    // <<------------[functions used in callback]------>>
    string generate_uuid();
    string generate_signature(const string& message);
    string get_current_binance_timestamp();
    string prepare_order_payload(const string& method, const string& price, const string& side, const string& cliId, const string& timestamp);
    string close_order_payload(const string& price, const string& side, const string& cliId, const string& timestamp) {
        string pos = (side == "BUY") ? "LONG" : "SHORT";
        string SIDE = (side == "BUY") ? "SELL" : "BUY";

        // Use the actual order quantity instead of hardcoded "0.01"
        string quantity = "0.001"; // Should match your order creation quantity

        string params =
            "apiKey=" + API_KEY +
            "&newClientOrderId=" + cliId +
            "&positionSide=" + pos +
            "&price=" + price +
            "&quantity=" + quantity +
			"&recvWindow=5000" + 
            "&side=" + SIDE +
            "&symbol=BTCUSDT" +
            "&timeInForce=GTC" +
            "&timestamp=" + timestamp +
            "&type=LIMIT";

        std::string signature = generate_signature(params);

        return R"({
            "id": ")" + generate_uuid() + R"(",
            "method": "order.place",
            "params": {
                "apiKey": ")" + API_KEY + R"(",
                "newClientOrderId": ")" + cliId + R"(",
                "positionSide": ")" + pos + R"(",
                "price": ")" + price + R"(",
                "quantity": ")" + quantity + R"(",
                "recvWindow": 5000,
                "side": ")" + SIDE + R"(",
                "symbol": "BTCUSDT",
                "timeInForce": "GTC",
                "timestamp": )" + timestamp + R"(,
                "type": "LIMIT",
                "signature": ")" + signature + R"("
            }
        })";
    }
    string modify_order_payload(const string& price, const string& side, const string& order_id, const string& timestamp) {
        string pos = (side == "BUY") ? "LONG" : "SHORT";
        string SIDE = (side == "BUY") ? "BUY" : "SELL";

        // Use the actual order quantity instead of hardcoded "0.01"
        string quantity = "0.001"; // Should match your order creation quantity

        string params =
            "apiKey=" + API_KEY +
            "&origClientOrderId=" + order_id +
            "&origType=LIMIT"
            "&positionSide=" + pos +
            "&price=" + price +
            "&priceMatch=NONE" +
            "&quantity=" + quantity +
            "&side=" + SIDE +
            "&symbol=BTCUSDT" +
            "&timestamp=" + timestamp;

        string signature = generate_signature(params);

        return R"({
            "id": ")" + generate_uuid() + R"(",
            "method": "order.modify",
            "params": {
                "apiKey": ")" + API_KEY + R"(",
                "origClientOrderId": ")" + order_id + R"(",
                "origType": "LIMIT",
                "positionSide": ")" + pos + R"(",
                "price": ")" + price + R"(",
                "priceMatch" : "NONE",
                "quantity": ")" + quantity + R"(",
                "side": ")" + SIDE + R"(",
                "symbol": "BTCUSDT",
                "timestamp": )" + timestamp + R"(,
                "signature": ")" + signature + R"("
            }
        })";
    }
    
    void connect_to_binance(EndpointType type, const string& path);

    ConnectionInfo* getConnection(lws* wsi) {
        lock_guard<mutex> lock(connection_mutex);
        auto it = find_if(connections.begin(), connections.end(),
            [wsi](const ConnectionInfo& ci) { return ci.wsi == wsi; });
        return it != connections.end() ? &(*it) : nullptr;
    }

    int handleTestnet(lws* wsi, lws_callback_reasons reason, void* in, size_t len, ConnectionInfo& conn) {
        switch (reason) {
        case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: {
            unsigned char** p = (unsigned char**)in;
            unsigned char* end = (*p) + len;
            char buf[256];

            // Add API key header
            int n = snprintf(buf, sizeof(buf), "X-MBX-APIKEY: %s", API_KEY.c_str());
            if (n < 0 || n >= (int)sizeof(buf) ||
                lws_add_http_header_by_name(wsi,
                    (unsigned char*)"X-MBX-APIKEY",
                    (unsigned char*)API_KEY.c_str(),
                    API_KEY.length(), p, end)) {
                std::cerr << "[ERROR] Failed to add API key header" << std::endl;
                return -1;
            }
            break;
        }
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            cout << "Testnet connected: " << conn.path << endl;
            binance_connection = wsi;

            sendTestnetInitialization(wsi);
            lws_callback_on_writable(wsi);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            processTestnetMessage(string((char*)in, len));
			broadcastToAllClients(string((char*)in, len));
            break;

        case LWS_CALLBACK_CLIENT_WRITEABLE: {
            lock_guard<mutex> lock(queue_mutex);
            if (order_queue.empty()) {
                write_pending = false;
                cout << "[DEBUG] Writeable but queue empty" << endl;
                break;
            }

            auto [order_id, payload] = order_queue.front();

            vector<unsigned char> buf(LWS_PRE + payload.size());
            memcpy(buf.data() + LWS_PRE, payload.c_str(), payload.size());

            int written = lws_write(wsi, buf.data() + LWS_PRE, payload.size(), LWS_WRITE_TEXT);

            if (written < 0) {
                cerr << "[ERROR] Failed to send order " << order_id << endl;
                break;
            }

            order_queue.pop_front();
            cout << "[SENT] Order " << order_id << ". Remaining: " << order_queue.size() << endl;

            if (!order_queue.empty()) {
                lws_callback_on_writable(wsi);
            }
            else {
                write_pending = false;
            }
            break;
        }
        default:
            break;
        }
        return 0;
    }
  
    void debug_queue_state() {
        lock_guard<mutex> lock(queue_mutex);
        cout << "Queue State: " << order_queue.size() << " pending orders" << endl;
        if (!order_queue.empty()) {
            cout << "Oldest order: " << order_queue.front().first << endl;
        }
    }

    void debug_connection_state() {
        lock_guard<mutex> lock(connection_mutex);
        cout << "Connection: " << (binance_connection ? "Valid" : "NULL") << endl;
        if (binance_connection) {
            cout << "State: " << lws_get_context(binance_connection) << endl;
        }
    }
    
    int handleUserData(lws* wsi, lws_callback_reasons reason, void* in, size_t len, ConnectionInfo& conn) {
        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            cout << "User data connected" << endl;
            subscribeToUserStream(wsi);
            thread([this, wsi]() {
                while (is_running()) {
                    std::this_thread::sleep_for(std::chrono::minutes(29));
                    subscribeToUserStream(wsi);
                }
                }).detach();
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            processUserDataUpdate(string((char*)in, len));
            break;

        default:
            break;
        }
        return 0;
    }

    int handleMarketData(lws* wsi, lws_callback_reasons reason, void* in, size_t len, ConnectionInfo& conn) {
        switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            cout << "Market data connected: " << conn.path << endl;
            subscribeToMarketStream(wsi, conn.path);
            break;

        case LWS_CALLBACK_CLIENT_RECEIVE:
            processMarketData(string((char*)in, len));
            break;

        default:
            break;
        }
        return 0;
    }

    // Helper methods
    string getEndpointForType(EndpointType type) {
        switch (type) {
        case EndpointType::TESTNET: return "testnet.binancefuture.com";
        case EndpointType::USER_DATA: return "stream.binancefuture.com";
        case EndpointType::MARKET_DATA: return "fstream.binancefuture.com";
        default: return "";
        }
    }

    void sendTestnetInitialization(lws* wsi) {
        const char* msg = R"({"id":"time_check","method":"time"})";
        sendMessage(wsi, msg);
    }

    void subscribeToUserStream(lws* wsi) {
        const char* msg = R"({"method":"SUBSCRIBE","params":["userDataStream"]})";
        sendMessage(wsi, msg);
    }

    void subscribeToMarketStream(lws* wsi, const string& stream) {
        string msg = R"({"method":"SUBSCRIBE","params":[")" + stream + R"("]})";
        sendMessage(wsi, msg.c_str());
    }

    void sendMessage(lws* wsi, const char* msg) {
        size_t msg_len = strlen(msg);
        std::vector<unsigned char> buf(LWS_PRE + msg_len);
        memcpy(buf.data() + LWS_PRE, msg, msg_len);
        lws_write(wsi, buf.data() + LWS_PRE, msg_len, LWS_WRITE_TEXT);
    }

    // Message processors
    void processTestnetMessage(const std::string& msg) {
        simdjson::ondemand::parser parser;
        simdjson::padded_string json(msg);

        auto doc = parser.iterate(json);
        if (doc.error()) {
            std::cerr << "JSON parse error: " << simdjson::error_message(doc.error()) << std::endl;
            return;
        }

        try {
            // Required fields
            auto result = doc["result"];
            int64_t orderId = result["orderId"].get_int64().value();
            std::string_view symbol = result["symbol"].get_string().value();
            std::string_view status = result["status"].get_string().value();

            // Optional fields (with defaults)
            std::string_view price = result["price"].get_string().value();
            std::string_view side = result["side"].get_string().value();

            std::cout << "Order ID: " << orderId << "\n"
                << "Symbol: " << symbol << "\n"
                << "Status: " << status << "\n"
                << "Price: " << price << "\n"
                << "Side: " << side << "\n";
        }
        catch (const simdjson::simdjson_error& e) {
            cout << "MESSAGE CAUSED ERROR" << msg << endl;
            std::cerr << "SIMDJSON error: " << e.what() << std::endl;
        }
    }

    void processUserDataUpdate(const string& msg) {
        //cout << "User Data: " << msg << endl;
        simdjson::dom::parser parser;
        simdjson::dom::element element;

        auto error = parser.parse(msg).get(element);
        if (error != simdjson::SUCCESS) {
            cerr << "JSON parse error: " << simdjson::error_message(error) << endl;
            return;
        }

        // Skip control messages
        simdjson::dom::element event_element;
        if (element["e"].get(event_element) != simdjson::SUCCESS) {
            return;
        }

        string_view event_type;
        if (event_element.get(event_type) != simdjson::SUCCESS) {
            cerr << "Failed to extract event type as string" << endl;
            return;
        }
        // Handle order reports
        if (event_type == "ORDER_TRADE_UPDATE") {
            processOrderUpdate(element);
        }
    }

    void processMarketData(const string& msg) {
        cout << "Market Data: " << msg << endl;
        // Parse price updates, order book changes, etc.
    }

    void processOrderUpdate(simdjson::dom::element& result) {
        bool should_check_filled = false;
        try {
            // Variables to hold extracted info outside the lock if needed
            string client_order_id_str;

            {
                lock_guard<mutex> lock(connection_mutex);

                // Extract the order object
                simdjson::dom::element order_element;
                if (result["o"].get(order_element) != simdjson::SUCCESS) {
                    cerr << "Failed to get order data from update" << endl;
                    return;
                }

                // Extract basic order information
                string_view client_order_id;
                if (order_element["c"].get(client_order_id) != simdjson::SUCCESS) {
                    cerr << "Failed to get client order ID" << endl;
                    return;
                }
                if (client_order_id.length() > 200) {
                    cerr << "client_order_id too long, skipping: " << client_order_id.length() << endl;
                    return;
                }
                client_order_id_str = string(client_order_id);

                // Find the order in our tracking
                auto order_it = pending_orders.find(client_order_id_str);
                if (order_it == pending_orders.end()) {

                    order_it = open_orders.find(client_order_id_str);
                    if (order_it == open_orders.end()) {

                        order_it = orders_to_close.find(client_order_id_str);
                        if (order_it == orders_to_close.end()) {
                            cout << "[CLOSED Order]: and removed " << client_order_id << endl;
                            return;
                        }
                    }
                    else {
                        cout << "Order is found in open orders queue" << endl;
                    }
                }

                Order& order = order_it->second;

                // parse side (BUY/SELL)
                string_view side;
                if (order_element["S"].get(side) == simdjson::SUCCESS) {
                    cout << "Side: " << side << endl;
                }

                // Parse position side (LONG/SHORT)
                string_view position;
                if (order_element["ps"].get(position) == simdjson::SUCCESS) {
                    cout << "Position: " << position << endl;
                }

                // Update order status
                string_view status_str;
                if (order_element["X"].get(status_str) == simdjson::SUCCESS) {
                    if (status_str == "NEW") {
                        order.status = OrderStatus::NEW;
                        should_check_filled = true;
                    }
                    else if (status_str == "FILLED") {
                        order.status = OrderStatus::FILLED;
                        order.executedQty = order.quantity; // Fully filled
                        
                        if ((side == "BUY" && position == "SHORT") || (side == "SELL" && position == "LONG")) {
                            cout << "Received a Close order for ID (" << order.client_id << ")" << endl;
                            closed_order_count[order.client_id] = order;
							orders_to_close.erase(order.client_id);
							open_orders.erase(order.client_id); // Remove from open orders
                            if (getOpenOrdersCount() == 0) {
                                iterations = 0; // Reset to allow new orders
                                cout << "[REQUOTING] Ready for new orders" << endl;
                            }
                        }

                        should_check_filled = true;
                    }
                    else if (status_str == "PARTIALLY_FILLED") {
                        order.status = OrderStatus::FILLED; // Treat partial as filled for simplicity
                    }
                    else if (status_str == "REJECTED") {
                        order.status = OrderStatus::CANCELED;
                    }
                    else if (order.status == OrderStatus::CANCELED || order.status == OrderStatus::EXPIRED) {
                        open_orders.erase(order_it);  // Prevent memory leaks
                    }
                }

                // Update execution quantities
                double executed_qty = 0.0;
                if (order_element["l"].get(executed_qty) == simdjson::SUCCESS) {
                    order.executedQty = executed_qty;
                }

                double cummulative_quote_qty = 0.0;
                if (order_element["z"].get(cummulative_quote_qty) == simdjson::SUCCESS) {
                    order.cummulativeQuoteQty = cummulative_quote_qty;
                }

                // Update order ID if this is the first acknowledgement                
                int64_t binance_order_id = 0;
                if (order_element["i"].get(binance_order_id) == simdjson::SUCCESS) {
                    order.ID = binance_order_id;
                }

                // Process fills if this is a partial or full fill
                string_view status_type_str;
                if (order_element["x"].get(status_type_str) == simdjson::SUCCESS &&
                    (status_type_str == "TRADE" || status_type_str == "PARTIALLY_FILLED")) {

                    double last_filled_price = 0.0;
                    double last_filled_qty = 0.0;

                    if (order_element["L"].get(last_filled_price) == simdjson::SUCCESS &&
                        order_element["l"].get(last_filled_qty) == simdjson::SUCCESS) {

                        order.fills.emplace_back(last_filled_price, last_filled_qty);
                        bool should_check_filled = true;
                        // For market data integration, you might want to update something here
                        cout << "[FILL] Order " << client_order_id
                            << " filled " << last_filled_qty
                            << " at price " << last_filled_price << endl;
                    }
                }
                else if (order_element["x"].get(status_type_str) == simdjson::SUCCESS &&
                    status_type_str == "AMENDMENT") {
                    should_check_filled = true;
                }

                // Log important state changes
                if (order.status == OrderStatus::FILLED) {
                    cout << "[FILLED] Order " << client_order_id
                        << " completely filled. Total cost: " << fixed << setprecision(8)
                        << order.cummulativeQuoteQty << endl;

                }
                else if (order.status == OrderStatus::CANCELED ||
                    order.status == OrderStatus::EXPIRED) {
                    cout << "[CANCELED/FAILED] Order " << client_order_id
                        << " status: " << status_str << endl;

                }
            }
            if (should_check_filled) {
                check_filled_orders();
                should_check_filled = false;
            }
        }
        catch (const exception& e) {
            cerr << "Exception processing order update: " << e.what() << endl;
        }
    }


    // Mutexs & Flages
    mutex time_mutex;
    std::mutex connection_mutex;
    atomic<long long> last_time_sync;
    atomic <long long> time_drift;
    atomic<bool> running;

    // Global Prices
    Market_metrics_manager* metrics_manager = nullptr;

    // Websocket Structs
    struct lws_context* context;
    struct lws* binance_connection = nullptr;

    // WebSocket Connections
    vector<ConnectionInfo> connections;
    vector<struct lws*> client_connections;
    
    // Track all connected clients
    mutex clients_mutex;
    
    //order vectors
    vector<string> orders_to_remove;
    
    deque<pair<string, string>> order_queue;  // <order_id, payload>
    mutex queue_mutex;
    condition_variable queue_cv;
    atomic<bool> write_pending{ false };

    // Unordered Maps for Orders
    unordered_map<string, Order> pending_orders, open_orders,
        orders_to_modify, orders_to_close,
        orders_to_cancel, closed_order_count;
    unordered_map<string, pair<Order*, Order*>> order_pairs;

    // Binance connector class
    const int PORT = 9443;
    string API_KEY = "ff669fdbbf5aca7da0626a35e84be1070c6d235c3f24c98f4140ee943d2d9995";
    string API_SECRET = "34e748ce5159f515d40099c80f05a2a772d48f5e1e90a84303fc2aa66e175033";
};

