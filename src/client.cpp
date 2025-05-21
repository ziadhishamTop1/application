#include <libwebsockets.h>
//#include <main.cpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <csignal>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <queue>
#include "persistanceSharedRingBuffer.hpp"

// Connection state and priority enums
enum class ConnectionState {
    DISCONNECTED, CONNECTING, CONNECTED, CONNECTION_ERROR
};

enum class ConnectionPriority {
    HIGH, MEDIUM, LOW
};

// Performance metrics structure
struct PerformanceStats {
    std::atomic<size_t> messages_received{ 0 };
    std::atomic<size_t> connection_errors{ 0 };
    std::atomic<size_t> bytes_received{ 0 };
    std::chrono::steady_clock::time_point start_time;
};

// WebSocket connection class
class WebSocketConnection {
public:
    WebSocketConnection(const std::string& symbol, ConnectionPriority priority)
        : symbol(symbol), priority(priority) {
        message_buffer.reserve(8192); // Pre-allocate buffer
    }

    ~WebSocketConnection() {
        if (wsi) {
            lws_set_wsi_user(wsi, nullptr);
        }
    }

    // Connection properties
    std::atomic<ConnectionState> state{ ConnectionState::DISCONNECTED };
    std::vector<char> message_buffer;
    std::string symbol;
    ConnectionPriority priority;
    struct lws* wsi{ nullptr };
    std::chrono::steady_clock::time_point connect_time;
    std::chrono::seconds reconnect_delay{ 1 };
    size_t reconnect_attempts{ 0 };
};

// Global variables
std::atomic<bool> running(true);
struct lws_context* context = nullptr;
std::mutex connection_mutex;
PerformanceStats stats;
std::unordered_map<struct lws*, std::shared_ptr<WebSocketConnection>> connections;
std::queue<std::shared_ptr<WebSocketConnection>> connection_pool;

// Callback function
static int callback(struct lws* wsi, enum lws_callback_reasons reason,
    void* user, void* in, size_t len) {
    WebSocketConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        auto it = connections.find(wsi);
        if (it != connections.end()) {
            conn = it->second.get();
        }
    }

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        if (conn) {
            conn->state = ConnectionState::CONNECTED;
            conn->reconnect_attempts = 0;
            conn->reconnect_delay = std::chrono::seconds(1);
            std::cout << "Connected to " << conn->symbol << " WebSocket" << std::endl;

            // Subscription message for order book, aggTrade, and mark price
            std::string msg = R"({
                "method": "SUBSCRIBE",
                "params": [
                    ")" + conn->symbol + R"(@trade",
                    ")" + conn->symbol + R"(@depth20@100ms"
                ],
                "id": 1
            })";

            /*std::string msg = R"({
                "method": "SUBSCRIBE",
                "params": [
                    ")" + conn->symbol + R"(@trade",
                    //")" + conn->symbol + R"(@aggTrade",
                    //")" + conn->symbol + R"(@depth20@100ms",
                    //*")" + conn->symbol + R"(@kline_1m"
                ],
                "id": 1
            })";*/

            std::vector<unsigned char> buf(LWS_PRE + msg.size());
            std::memcpy(buf.data() + LWS_PRE, msg.c_str(), msg.size());
            lws_write(wsi, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
        }
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        if (conn) {
            conn->state = ConnectionState::CONNECTION_ERROR;
            stats.connection_errors++;
            std::cerr << "Connection error for " << conn->symbol << std::endl;
        }
        break;

    case LWS_CALLBACK_CLIENT_CLOSED:
        if (conn) {
            conn->state = ConnectionState::DISCONNECTED;
            std::cout << "Connection closed for " << conn->symbol << std::endl;
        }
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        if (conn && len > 0) {
            stats.messages_received++;
            stats.bytes_received += len;
            string json((char*)in, len);
			//cout << "[RAW] response : " << json << endl;
			// Process incoming JSON data
			handleIncomingData(json);
        } break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        if (conn && conn->state == ConnectionState::CONNECTED) {
            // send ping every 15 second
            auto now = std::chrono::steady_clock::now();
            if (now - conn->connect_time > std::chrono::seconds(15)) {
                unsigned char ping[LWS_PRE + 125] = { 0 };
                lws_write(wsi, ping + LWS_PRE, 0, LWS_WRITE_PING);
                conn->connect_time = now;
            }
        }break;

    default:
        break;
    }return 0;
}

// proto definitions 
static struct lws_protocols protocols[] = {
    {"binance-protocol", callback, 0, 0},
    {nullptr, nullptr, 0, 0}
};

// Connection pool managment
std::shared_ptr<WebSocketConnection> create_or_reuse_connection(
    const std::string& symbol, ConnectionPriority priority) {
    std::lock_guard<std::mutex> lock(connection_mutex);

    if (!connection_pool.empty()) {
        auto conn = connection_pool.front();
        connection_pool.pop();
        conn->symbol = symbol;
        conn->priority = priority;
        conn->state = ConnectionState::DISCONNECTED;
        conn->message_buffer.clear();
        return conn;
    }return std::make_shared<WebSocketConnection>(symbol, priority);
}

void release_connection(std::shared_ptr<WebSocketConnection> conn) {
    std::lock_guard<std::mutex> lock(connection_mutex);
    connection_pool.push(conn);
}

// WebSocket connection function
void connectWebSocket(const std::string& symbol,
    ConnectionPriority priority = ConnectionPriority::MEDIUM) {
    static std::chrono::steady_clock::time_point last_connect;
    auto now = std::chrono::steady_clock::now();

    // Rate limiting - Max 2 connections per seconds 
    if (now - last_connect < std::chrono::milliseconds(500)) {
        std::this_thread::sleep_until(last_connect + std::chrono::milliseconds(500));
    }
    last_connect = std::chrono::steady_clock::now();

    auto conn = create_or_reuse_connection(symbol, priority);
    conn->connect_time = std::chrono::steady_clock::now();
    conn->state = ConnectionState::CONNECTING;

    struct lws_client_connect_info ccinfo = {};
    ccinfo.context = context;
    ccinfo.address = "fstream.binancefuture.com";
    ccinfo.port = 443;
    std::string path = "/ws/" + symbol + "@depth100";
    ccinfo.path = path.c_str();
    ccinfo.host = ccinfo.address;
    ccinfo.origin = ccinfo.address;
    ccinfo.protocol = protocols[0].name; // e.g., "Binance-protocols"
    ccinfo.ssl_connection = LCCSCF_USE_SSL;
    ccinfo.userdata = conn.get();

    struct lws* wsi = lws_client_connect_via_info(&ccinfo);
    if (!wsi) {
        std::cerr << "Failed to connect to" << symbol << " Websocket" << std::endl;
        conn->state = ConnectionState::CONNECTION_ERROR;
        release_connection(conn);
    } else {
        conn->wsi = wsi;
        std::lock_guard<std::mutex> lock(connection_mutex);
        connections[wsi] = conn;
    }
}

// Reconnection managment
void manage_connections() {
    std::vector<std::pair<std::string, ConnectionPriority>> to_reconnect;
    std::vector<struct lws*> to_remove;

    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        for (auto& [wsi, conn] : connections) {
            // handle timeouts
            auto now = std::chrono::steady_clock::now();
            if (conn->state == ConnectionState::CONNECTING &&
                (now - conn->connect_time) > std::chrono::seconds(10)) {
                conn->state = ConnectionState::CONNECTION_ERROR;
            }

            // Schedule reconnections
            if (conn->state == ConnectionState::DISCONNECTED || 
                conn->state == ConnectionState::CONNECTION_ERROR) {
                if (conn->reconnect_attempts < 5) {
                    to_reconnect.emplace_back(conn->symbol, conn->priority);
                    conn->reconnect_attempts++;
                    conn->reconnect_delay = std::min(
                        conn->reconnect_delay * 2, std::chrono::seconds(15));
                }to_remove.push_back(wsi);
            }
        }

        // Remove dead connections
        for (auto wsi : to_remove) {
            if (connections.find(wsi) != connections.end()) {
                release_connection(connections[wsi]);
                connections.erase(wsi);
            }
        }
    }

    // Reconnect with proper delays 
    for (const auto& [symbol, priority] : to_reconnect) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        connectWebSocket(symbol, priority);
    }
}   

// Signal handler for gracefull shutdown
void signal_handler(int signum) {
    std::cout << "Received signal " << signum << ", shutting down..." << std::endl;
    running = false;
}

// print statistics 
void print_stats() {
    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - stats.start_time);

    std::cout << "\n=== statistics ===" << std::endl;
    std::cout << "Uptime: " << uptime.count() << " seconds" << std::endl;
    std::cout << "Messages received: " << stats.messages_received << std::endl;
    std::cout << "Data received: " << stats.bytes_received / 1024 << " KB" << std::endl;
    std::cout << "Connection error: " << stats.connection_errors << std::endl;

    size_t connected = 0;
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        for (const auto& [wsi, conn] : connections) {
            if (conn->state == ConnectionState::CONNECTED) connected++;
        }
    }std::cout << "Active connections: " << connected << std::endl;
}

int main() {
    //set up signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Initialize context
    struct lws_context_creation_info info = {};
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT |
        LWS_SERVER_OPTION_SKIP_SERVER_CANONICAL_NAME;
    info.fd_limit_per_thread = 256;

    context = lws_create_context(&info);
    if (!context) {
        std::cerr << "Failed to create WebSocket context" << std::endl;
        return 1;
    }

    stats.start_time = std::chrono::steady_clock::now();

    // Iniitial connections
    std::vector<std::pair<std::string, ConnectionPriority>> symbol = {
        {"btcusdt", ConnectionPriority::HIGH}
        /*{"ethusdt", ConnectionPriority::HIGH},
        {"bnbusdt", ConnectionPriority::MEDIUM}*/
    };

    for (const auto& [symbol, priority] : symbol) {
        connectWebSocket(symbol, priority);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // Main loop
    while (running) {
        lws_service(context, 50); // 50ms timeout

        // Periodic maintainance 
        static auto last_maintenance = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (now - last_maintenance > std::chrono::seconds(30)) {
            manage_connections();

            // Print stats every 30 seconds
            if (now - stats.start_time > std::chrono::seconds(30)) {
                print_stats();
            }
            last_maintenance = now;
        }
    }

    // Clean up
    {
        std::lock_guard<std::mutex> lock(connection_mutex);
        connections.clear();
        connection_pool = std::queue<std::shared_ptr<WebSocketConnection>>();
    }

    lws_context_destroy(context);
    print_stats();
    return 0;
}
