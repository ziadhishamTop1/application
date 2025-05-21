#pragma once
#include <iostream>
#include <Windows.h>
#include <chrono>
#include <atomic>
#include <csignal>
#include <thread>
#include <charconv>
#include <simdjson.h>
#include <flatbuffers/flatbuffers.h>
#include "buffers_generated.h"

using namespace std;

class binanceParserToFlatbuffers {
public:
    // Flag for unhandled messages
    bool is_valid = false;

    // Constructor
    binanceParserToFlatbuffers(const string& json)
        : builder(make_unique<flatbuffers::FlatBufferBuilder>()) {

        try {
            simdjson::dom::parser parser;
            simdjson::dom::element element;

            // Safely parse JSON with error handling
            auto parse_error = parser.parse(json).get(element);
            if (parse_error) {
                cout << "JSON parse error, skipping message" << endl;
                return;
            }

            // Safely get event type with error handling
            simdjson::dom::element event_type_element;
            string_view event_type;

            auto& builder = *this->builder; // GETTING THE BUILDER

            // Ignore control/heartbeat/serverTime messages early
            simdjson::dom::element result;
            if (element["result"].get(result) == simdjson::SUCCESS) {
                if (result["serverTime"].error() == simdjson::SUCCESS) {
                    cout << "Control or server message, skipping: " << json << endl;
                    return;
                }
                else if (result["orderId"].error() == simdjson::SUCCESS &&
                    result["clientOrderId"].error() == simdjson::SUCCESS) {
                    if (!processOderReports(element, builder)) return;
                    is_valid = true;
                    return;
                }
            }

            // Filtering the message depending on it's type
            if (element["lastUpdate"].error() == simdjson::SUCCESS) {
                if (!processDepthUpdate(element, builder)) return;
                is_valid = true;
            }
            
            if (element["e"].get(event_type_element) != simdjson::SUCCESS) {
                cout << "Message missing event type, skipping: " << json << endl;
                return;
            }

            if (event_type_element.get_string().get(event_type) != simdjson::SUCCESS) {
                cout << "Invalid event type format, skipping: " << json << endl;
                return;
            }
            
            if (event_type == "trade") {
                if (!processTrade(element, builder)) return;
                is_valid = true;
            }
            else if (event_type == "depthUpdate") {
                if (!processDepthUpdate(element, builder)) return;
                is_valid = true;
            }
            else {
                cout << "Unhandled event type, skipping: " << event_type << endl;
            }
        }
        catch (const exception& e) {
            cerr << "Parser exception, skipping message: " << e.what() << endl;
        }
    }

    // return the buffer
    std::pair<const uint8_t*, size_t> getBuffer() const {
        if (!is_valid || !builder) return { nullptr, 0 };
        return { builder->GetBufferPointer(), builder->GetSize() };
    }

    bool processDepthUpdate(const simdjson::dom::element& element, flatbuffers::FlatBufferBuilder& builder) {
        try {
            // Get current timestamp for event_time and timestamp fields
            uint64_t event_time = chrono::duration_cast<chrono::milliseconds>(
                chrono::system_clock::now().time_since_epoch()).count();
            uint64_t timestamp = event_time; // Using same value for both fields

            // Get lastUpdateId (now using lowercase 'lastUpdateId' to match new payload)
            uint64_t last_update_id;
            if (element["u"].get_uint64().get(last_update_id) != simdjson::SUCCESS) {
                cout << "Missing lastUpdateId in depthUpdate" << endl;
                return false;
            }

            // Create header string (hardcoded as in original)
            auto header = builder.CreateString("header");
            /*cout << "Event Type: depthUpdate" << endl;
            cout << "Event Time: " << event_time << endl;
            cout << "Timestamp: " << timestamp << endl;
            cout << "Last Update ID: " << last_update_id << endl;*/

            vector<flatbuffers::Offset<Binance::fb::PriceLevel>> bids, asks;

            // Process bids (array is now called "bids" instead of "b")
            simdjson::dom::array bids_array;
            if (element["b"].get_array().get(bids_array) == simdjson::SUCCESS) {
                for (auto bid : bids_array) {
                    double price, quantity;
                    string_view price_str, qty_str;

                    if (bid.at(0).get_string().get(price_str) == simdjson::SUCCESS &&
                        bid.at(1).get_string().get(qty_str) == simdjson::SUCCESS) {

                        if (from_chars(price_str.data(), price_str.data() + price_str.size(), price).ec == errc{} &&
                            from_chars(qty_str.data(), qty_str.data() + qty_str.size(), quantity).ec == errc{}) {

                            auto price_level = Binance::fb::CreatePriceLevel(builder, price, quantity);
                            bids.push_back(price_level);
                            cout << "Bid Price: " << fixed << setprecision(2) << price << ", Quantity: " << quantity << endl;
                        }
                    }
                }
            }

            // Process asks (array is now called "asks" instead of "a")
            simdjson::dom::array asks_array;
            if (element["a"].get_array().get(asks_array) == simdjson::SUCCESS) {
                for (auto ask : asks_array) {
                    double price, quantity;
                    string_view price_str, qty_str;

                    if (ask.at(0).get_string().get(price_str) == simdjson::SUCCESS &&
                        ask.at(1).get_string().get(qty_str) == simdjson::SUCCESS) {

                        if (from_chars(price_str.data(), price_str.data() + price_str.size(), price).ec == errc{} &&
                            from_chars(qty_str.data(), qty_str.data() + qty_str.size(), quantity).ec == errc{}) {

                            auto price_level = Binance::fb::CreatePriceLevel(builder, price, quantity);
                            asks.push_back(price_level);
                            cout << "Ask Price: " << fixed << setprecision(2) << price << ", Quantity: " << quantity << endl;
                        }
                    }
                }
            }

            // Create FlatBuffers objects
            auto symbol_offset = builder.CreateString("symbol"); // Hardcoded symbol as in original
            auto bids_offset = builder.CreateVector(bids);
            auto asks_offset = builder.CreateVector(asks);

            // Create depth update (keeping same structure as original)
            auto depth_update = Binance::fb::CreateDepthUpdate(
                builder,
                header,
                event_time,
                timestamp,
                last_update_id,
                symbol_offset,
                bids_offset,
                asks_offset
            );

            // Create and finish envelope (same as original)
            auto envelope = Binance::fb::CreateEnvelope(
                builder,
                Binance::fb::MessageType_DepthUpdate,
                Binance::fb::MessagePayload_DepthUpdate,
                depth_update.Union()
            );
            builder.Finish(envelope);

            // Test deserialization (same as original)
            auto buffer = builder.GetBufferPointer();
            auto envelope_ptr = Binance::fb::GetEnvelope(buffer);

            if (envelope_ptr->type() == Binance::fb::MessageType_DepthUpdate) {
                auto depth_update_ptr = envelope_ptr->payload_as<Binance::fb::DepthUpdate>();
                //cout << "Deserialized Symbol: " << depth_update_ptr->symbol()->str() << endl;
                //cout << "Deserialized Bids: " << depth_update_ptr->bids()->size() << endl;
                //cout << "Deserialized Asks: " << depth_update_ptr->asks()->size() << endl;

                double total_bids = 0.0, total_asks = 0.0;
                for (auto bid : *depth_update_ptr->bids()) {
                    cout << "Deserialized Bid Price: " << bid->price() << ", Quantity: " << bid->quantity() << endl;
                    total_bids += bid->quantity();
                }
                for (auto ask : *depth_update_ptr->asks()) {
                    cout << "Deserialized Ask Price: " << ask->price() << ", Quantity: " << ask->quantity() << endl;
                    total_asks += ask->quantity();
                }
                //cout << "Total Bids: " << total_bids << endl;
                //cout << "Total Asks: " << total_asks << endl;
            }

            return true;
        }
        catch (const exception& e) {
            cerr << "DepthUpdate processing error: " << e.what() << endl;
            return false;
        }
    }

    bool processTrade(const simdjson::dom::element& element, flatbuffers::FlatBufferBuilder& builder) {
    try {
        // Extract required fields with error checking
        string_view event_type = "trade"; // Hardcoded since we're in trade handler
        uint64_t event_time;
        if (element["E"].get_uint64().get(event_time) != simdjson::SUCCESS) {
            cout << "Missing event time in trade" << endl;
            return false;
        }

        string_view symbol;
        if (element["s"].get_string().get(symbol) != simdjson::SUCCESS) {
            cout << "Missing symbol in trade" << endl;
            return false;
        }

        uint64_t trade_id;
        if (element["t"].get_uint64().get(trade_id) != simdjson::SUCCESS) {
            cout << "Missing trade ID in trade" << endl;
            return false;
        }

        // Extract price and quantity
        double price = 0, quantity = 0;
        string_view price_str, qty_str;

        if (element["p"].get_string().get(price_str) != simdjson::SUCCESS ||
            element["q"].get_string().get(qty_str) != simdjson::SUCCESS) {
            cout << "Missing price or quantity in trade" << endl;
            return false;
        }

        auto price_result = from_chars(price_str.data(), price_str.data() + price_str.size(), price);
        auto qty_result = from_chars(qty_str.data(), qty_str.data() + qty_str.size(), quantity);
        
        if (price_result.ec != errc{} || qty_result.ec != errc{}) {
            cout << "Invalid price or quantity format" << endl;
            return false;
        }

        if (price <= 0 || quantity <= 0) {
            cout << "Invalid price or quantity value" << endl;
            return false;
        }

        // Extract additional fields from your schema
        uint64_t trade_time = event_time; // Using event time as trade time if not specified
        element["T"].get_uint64().get(trade_time); // Try to get trade time, fallback to event_time

        bool is_buyer_maker = false;
        element["m"].get_bool().get(is_buyer_maker); // Get if buyer is maker

        // Create trade using the direct method from your schema
        auto trade = Binance::fb::CreateTradeDirect(
            builder,
            event_type.data(), // event_type
            event_time,       // event_time
            symbol.data(),    // symbol
            trade_id,        // trade_id
            price,           // price
            quantity,        // quantity
            trade_time,      // trade_time
            is_buyer_maker   // is_buyer_maker
        );

        // Create envelope and finish
        auto envelope = Binance::fb::CreateEnvelope(
            builder,
            Binance::fb::MessageType_Trade,
            Binance::fb::MessagePayload_Trade,
            trade.Union()
        );
        builder.Finish(envelope);

        // Log successful processing
        cout << "Processed trade: " << symbol << " | Price: " << fixed << setprecision(2) << price
             << " | Qty: " << quantity << " | BuyerMaker: " << (is_buyer_maker ? "Yes" : "No") << endl;

        return true;
    }
    catch (const exception& e) {
        cerr << "Trade processing error: " << e.what() << endl;
        return false;
    }
}

    bool processOderReports(const simdjson::dom::element& element, flatbuffers::FlatBufferBuilder& builder) {
        try {
            simdjson::dom::element result;
            if (element["result"].get(result) == simdjson::SUCCESS) {
                uint64_t id;
                if (result["orderId"].get_uint64().get(id) != simdjson::SUCCESS) {
                    cout << "Missing order ID in order report" << endl;
                    return false;
                }

                string_view client_order_id;
                if (result["clientOrderId"].get_string().get(client_order_id) != simdjson::SUCCESS) {
                    cout << "Missing client order ID in order report" << endl;
                    return false;
                }

                string_view symbol;
                if (result["symbol"].get_string().get(symbol) != simdjson::SUCCESS) {
                    cout << "Missing symbol in order report" << endl;
                    return false;
                }

                string_view side;
                if (result["side"].get_string().get(side) != simdjson::SUCCESS) {
                    cout << "Missing side in order report" << endl;
                    return false;
                }

                string_view status;
                if (result["status"].get_string().get(status) != simdjson::SUCCESS) {
                    cout << "Missing status in order report" << endl;
                    return false;
                }

                double exeQty = 0;
                string_view executed_qty;
                if (result["executedQty"].get_string().get(executed_qty) != simdjson::SUCCESS) {
                    cout << "Missing price in order report" << endl;
                    return false;
                }

                auto price_result = from_chars(executed_qty.data(), executed_qty.data() + executed_qty.size(), exeQty);

                auto orderReport = Binance::fb::CreateorderReportDirect(
                    builder,
                    id,
                    client_order_id.data(),
                    symbol.data(),
                    side.data(),
                    status.data(),
                    exeQty
                );
                auto envelope = Binance::fb::CreateEnvelope(
                    builder,
                    Binance::fb::MessageType_orderReport,
                    Binance::fb::MessagePayload_orderReport,
                    orderReport.Union()
                );
                builder.Finish(envelope);
                cout << "Processed order report: " << symbol << endl
                    << "Order ID: " << id << endl
					<< "Side: " << side << endl
                    << "status: " << status << endl
                    << "Client ID: (" << client_order_id << ")" << endl;
                return true;
            }
        }
        catch (const exception& e) {
            cerr << "(Error) in parsing order reports" << endl;
            return false;
        }
    }
private:
    unique_ptr<flatbuffers::FlatBufferBuilder> builder;
};