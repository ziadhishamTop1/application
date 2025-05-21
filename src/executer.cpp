#include "persistanceSharedRingBuffer.hpp"
#include "executer.hpp"
#include <iostream>
#include <vector>
#include <libwebsockets.h>
#include <simdjson.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <rpc.h>
#include <iomanip>
#include <sstream>
#include <random>
#include <curl/curl.h>
#include <algorithm>
#include <winsock2.h>
#include "executer.hpp"
#include "Algorithm.hpp"

#pragma comment(lib, "Rpcrt4.lib")
#pragma comment(lib, "ws2_32.lib")

struct lws_protocols BinanceConnector::protocols[] = {
	{"binance-protocol", BinanceConnector::callback_binance, 0, 0},
	{nullptr, nullptr, 0, 0}
};
// constructor
BinanceConnector::BinanceConnector()
    : context(nullptr), binance_connection(nullptr), running(true) {}

// deconstructor
BinanceConnector::~BinanceConnector() {
	shutdown();
}

bool BinanceConnector::initialize() {
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cerr << "WSA-Startup [failed]: " << WSAGetLastError() << std::endl;
		return false;
    }

	struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
	info.port = 9443;
	info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
        LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

	// 👇 Allow external connections (optional)
	info.iface = NULL; // Listen on all interfaces (0.0.0.0)

	// Create the WebSocket context
	context = lws_create_context(&info);
    if (!context) {
        cerr << "[FAILED] to create websocket context" << endl;
        return false;
    }

	BinanceAPI api(API_KEY);
	listenKey = api.get_listen_key();
	lastKeyRenewal = std::chrono::system_clock::now();
	std::cout << "ListenKey: " << listenKey << std::endl;

    connect_to_binance(EndpointType::USER_DATA, ("/ws/" + listenKey).c_str());
	connect_to_binance(EndpointType::TESTNET, "/ws-fapi/v1");
	return true;
}

void BinanceConnector::connect_to_binance(EndpointType type, const string& path) {
	auto& instance = BinanceConnector::getInstance();    // Get instance
	string endpoint = getEndpointForType(type);
	struct lws_client_connect_info ccinfo = {};
	ccinfo.context = context; 
	ccinfo.address = endpoint.c_str();
	ccinfo.port = 443;
	ccinfo.path = path.c_str();
	ccinfo.host = ccinfo.address;
	ccinfo.origin = ccinfo.address;
	ccinfo.protocol = protocols[0].name;
	ccinfo.ssl_connection = LCCSCF_USE_SSL;

	struct lws* wsi = lws_client_connect_via_info(&ccinfo);
	if (!wsi) {
		cerr << "[FAILED] to connect to Binance" << endl;
		running = false;
	}
	else {
		lock_guard<mutex> lock(connection_mutex);
		connections.push_back({wsi, type, path});
		binance_connection = wsi;
	}
}

void BinanceConnector::shutdown() {
	if (context) {
		lws_context_destroy(context);
		context = nullptr;
	}
	WSACleanup();
	running = false;
}

void BinanceConnector::broadcastToAllClients(const std::string& message) {
	std::lock_guard<std::mutex> lock(clients_mutex);

	for (lws* client : client_connections) {
		if (client) {
			std::vector<unsigned char> buf(LWS_PRE + message.size());
			memcpy(buf.data() + LWS_PRE, message.c_str(), message.size());

			lws_write(client, buf.data() + LWS_PRE, message.size(), LWS_WRITE_TEXT);
			lws_callback_on_writable(client);
		}
	}
}

void BinanceConnector::broadcastToTestnetClients(const std::string& message) {
	std::lock_guard<std::mutex> lock(clients_mutex);

	for (lws* client : client_connections) {
		if (client) {
			ConnectionInfo* conn = getConnection(client);
			if (conn && conn->type == EndpointType::TESTNET) {
				std::vector<unsigned char> buf(LWS_PRE + message.size());
				memcpy(buf.data() + LWS_PRE, message.c_str(), message.size());

				lws_write(client, buf.data() + LWS_PRE, message.size(), LWS_WRITE_TEXT);
				lws_callback_on_writable(client);
			}
		}
	}
}

void BinanceConnector::broadcastSystemStatus() {
	std::string status = R"({
        "type": "system_status",
        "timestamp": )" + get_current_binance_timestamp() + R"(,
        "orders_queued": )" + std::to_string(order_queue.size()) + R"(,
        "open_orders": )" + std::to_string(open_orders.size()) + R"(,
        "uptime": ")" + /* Implement getUptimeString() if needed */ "0" + R"("
    })";

	broadcastToAllClients(status);
}

string BinanceConnector::generate_signature(const string& message) {
	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len;

	HMAC_CTX* ctx = HMAC_CTX_new();
	HMAC_Init_ex(ctx,
		reinterpret_cast<const unsigned char*>(API_SECRET.c_str()),
		API_SECRET.length(),
		EVP_sha256(),
		nullptr);

	HMAC_Update(ctx,
		reinterpret_cast<const unsigned char*>(message.c_str()),
		message.length());

	HMAC_Final(ctx, digest, &len);
	HMAC_CTX_free(ctx);

	stringstream ss;
	ss << hex << setfill('0');
	for (unsigned int i = 0; i < len; i++) {
		ss << setw(2) << static_cast<unsigned>(digest[i]);
	}
	return ss.str();
}

string BinanceConnector::generate_uuid() {
	UUID uuid;
	UuidCreate(&uuid);
	RPC_CSTR str;
	UuidToStringA(&uuid, &str);
	string uuid_str(reinterpret_cast<char*>(str));
	RpcStringFreeA(&str);
	return uuid_str;
}

string BinanceConnector::get_current_binance_timestamp() {
	CURL* curl = nullptr;
	string response;

	try {
		// 1. Initialize CURL with timeout
		curl = curl_easy_init();
		if (!curl) throw runtime_error("CURL initialization failed");

		curl_easy_setopt(curl, CURLOPT_URL, "https://testnet.binancefuture.com/fapi/v1/time");
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, BinanceAPI::WriteCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500); // 500ms timeout
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Prevent crashes in multi-threaded apps

		// 2. Execute request
		CURLcode res = curl_easy_perform(curl);
		if (res != CURLE_OK) {
			throw runtime_error("CURL error: " + string(curl_easy_strerror(res)));
		}

		// 3. Parse JSON safely
		simdjson::dom::parser parser;
		simdjson::dom::element doc;
		auto error = parser.parse(response).get(doc);
		if (error) throw runtime_error("JSON parse error: " + string(simdjson::error_message(error)));

		uint64_t server_time = doc["serverTime"].get_uint64().value();
		if (server_time == 0) throw runtime_error("Invalid server time");

		// 4. Calculate time drift (atomic for thread safety)
		auto local_ms = chrono::duration_cast<chrono::milliseconds>(
			chrono::system_clock::now().time_since_epoch()).count();
		time_drift.store(local_ms - server_time);

		return to_string(server_time + 100);

	}
	catch (const exception& e) {
		cerr << "[ERROR] Timestamp sync failed: " << e.what() << endl;
		// Fallback: Use local time if Binance API fails
		auto local_ms = chrono::duration_cast<chrono::milliseconds>(
			chrono::system_clock::now().time_since_epoch()).count();
		return to_string(local_ms - time_drift.load());
	}

	// 5. Cleanup CURL
	if (curl) curl_easy_cleanup(curl);
}

string BinanceConnector::prepare_order_payload(const string& method, const string& price, const string& side, const string& cliId, const string& timestamp) {
	string pos = (side == "BUY") ? "LONG" : "SHORT";
	const string quantity = "0.001";
	const string recvWindow = "5000";

	// Parameters for signature - must be alphabetical order
	std::string params =
		"apiKey=" + API_KEY +
		"&newClientOrderId=" + cliId +
		"&positionSide=" + pos +
		"&price=" + price +
		"&quantity=" + quantity +
		"&recvWindow=" + recvWindow +  // Add if using in signature
		"&side=" + side +
		"&symbol=BTCUSDT" +
		"&timeInForce=GTC" +
		"&timestamp=" + timestamp +
		"&type=LIMIT";

	std::string signature = generate_signature(params);

	// JSON payload
	return R"({
        "id": ")" + generate_uuid() + R"(",
        "method": "order.)" + method + R"(",
        "params": {
            "apiKey": ")" + API_KEY + R"(",
            "newClientOrderId": ")" + cliId + R"(",
            "positionSide": ")" + pos + R"(",
            "price": ")" + price + R"(",
            "quantity": ")" + quantity + R"(",
            "recvWindow": )" + recvWindow + R"(,
            "side": ")" + side + R"(",
            "symbol": "BTCUSDT",
            "timeInForce": "GTC",
            "timestamp": )" + timestamp + R"(,
            "type": "LIMIT",
            "signature": ")" + signature + R"("
        }
    })";
}

string BinanceConnector::create_order_pair(const string& bidExe, const string& askExe) {
	cout << "[INFO] Creating order pair" << endl;
	string round_trip_id = generate_uuid();
	string timestamp = get_current_binance_timestamp();

	// Create BUY order
	Order buy_order;
	buy_order.order_id = generate_uuid();
	buy_order.parent_id = round_trip_id;
	buy_order.client_id = "BTCH-" + to_string(batch_number.load()) + "-SYSTEM-" + buy_order.parent_id.substr(0, 5) + "-C-1";
	buy_order.symbol = "BTCUSDT";
	buy_order.side = OrderSide::BUY;
	buy_order.price = stod(bidExe);
	buy_order.quantity = 0.00847;
	buy_order.created = chrono::system_clock::now();
	buy_order.payload = prepare_order_payload("place", bidExe, "BUY", buy_order.client_id, timestamp);
	buy_order.status = OrderStatus::NEW;
	buy_order.needs_sending = true;
	buy_order.needs_closing = false;

	// Create SELL order
	Order sell_order;
	sell_order.order_id = generate_uuid();
	sell_order.parent_id = round_trip_id;
	sell_order.client_id = "BTCH-" + to_string(batch_number.load()) + "-SYSTEM-" + sell_order.parent_id.substr(0, 5) + "-C-2";
	sell_order.symbol = "BTCUSDT";
	sell_order.side = OrderSide::SELL;
	sell_order.price = stod(askExe);
	sell_order.quantity = 0.001;
	sell_order.created = chrono::system_clock::now();
	sell_order.payload = prepare_order_payload("place", askExe, "SELL", sell_order.client_id, timestamp);
	sell_order.status = OrderStatus::NEW;
	sell_order.needs_sending = true;
	sell_order.needs_closing = false;

	{
		lock_guard<mutex> lock(connection_mutex);
		pending_orders[buy_order.client_id] = buy_order;
		pending_orders[sell_order.client_id] = sell_order;
		cout << buy_order.payload << endl;
		cout << sell_order.payload << endl;
	}

	// Broadcast order creation to all clients
	string orderCreationMsg = R"({
        "event": "CREATE_ORDER_PAIR",
        "pair_id": ")" + round_trip_id + R"(",	
		"buyClientId": ")" + buy_order.client_id + R"(",
		"sellClientId": ")" + sell_order.client_id + R"(",
        "buy_price": )" + bidExe + R"(,
        "sell_price": )" + askExe + R"(,
        "quantity": 0.001,
        "timestamp": )" + timestamp + R"(
    })";

	broadcastToAllClients(orderCreationMsg);
	return round_trip_id;
}

int BinanceConnector::callback_binance(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len) {
	// Forward to the instance's handleCallback
	auto* instance = &BinanceConnector::getInstance();
	return instance->handleCallback(wsi, reason, in, len);
}

void BinanceConnector::check_filled_orders() {
	if (!metrics_manager) {
		cerr << "[ERROR] Metrics manager not set" << endl;
		return;
	}

	vector<pair<string, string>> pairs_to_close;
	{
		//lock_guard<mutex> lock(connection_mutex);

		// Build order pairs
		for (auto& [order_id, order] : open_orders) {
			if (!order.parent_id.empty()) {
				if (order.side == OrderSide::BUY) {
					order_pairs[order.parent_id].first = &order;
				}
				else {
					order_pairs[order.parent_id].second = &order;
				}
			}
		}

		// Check pairs for closing
		for (auto& [pair_id, orders] : order_pairs) {
			Order* buy_order = orders.first;
			Order* sell_order = orders.second;

			if (!buy_order || !sell_order) {
				cerr << "[ERROR] Incomplete pair: " << pair_id << endl;
				continue;
			}

			// Only proceed if BOTH orders are filled
			if (buy_order->status == OrderStatus::NEW &&
				sell_order->status == OrderStatus::NEW) {
				cout << "[INFO] Pair Not New In The Book: " << pair_id << endl;
				//if (chrono::system_clock::now() - buy_order->created > chrono::milliseconds(100)) {
				//	orders_to_modify[buy_order->client_id + "_MODIFIED"] = *buy_order;
				//	orders_to_modify[sell_order->client_id + "_MODIFIED"] = *sell_order;

				//	buy_order->created = chrono::system_clock::now();
				//	sell_order->created = chrono::system_clock::now();

				//	open_orders[buy_order->client_id + "_MODIFIED"] = *buy_order;
				//	open_orders[sell_order->client_id + "_MODIFIED"] = *sell_order;

				//	// Remove from open orders
				//	open_orders.erase(buy_order->client_id);
				//	open_orders.erase(sell_order->client_id);

				//	processModifyingOrders(*metrics_manager);
				//}
			}
			else if (buy_order->status == OrderStatus::FILLED &&
				sell_order->status == OrderStatus::FILLED) {

				cout << "[INFO] Closing pair: " << pair_id << "(BUY: " << buy_order->client_id << " )"
					<< " (SELL: " << sell_order->client_id << " )" << endl;

				// Mark orders for closing
				buy_order->needs_closing = true;
				sell_order->needs_closing = true;

				// Move to closing map
				orders_to_close[buy_order->client_id + "_CLOSE"] = *buy_order;
				orders_to_close[sell_order->client_id + "_CLOSE"] = *sell_order;

				// Remove from open orders
				open_orders.erase(buy_order->client_id);
				open_orders.erase(sell_order->client_id);

				processClosingOrder(*metrics_manager);
			}
		}
	}
}

struct lws* BinanceConnector::get_connection() {
	return binance_connection;
}