#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/gateway_adapter.hpp"
#include "kabu_micro_edge/gateway_types.hpp"

namespace kabu::gateway {

inline bool is_margin_mode(const std::string& mode) {
    static const std::unordered_set<std::string> margin_modes{
        "margin", "margin_daytrade", "margin_general", "credit", "shinyo"
    };
    std::string normalized = mode;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (normalized.empty() || normalized == "cash" || normalized == "spot") {
        return false;
    }
    if (margin_modes.contains(normalized)) {
        return true;
    }
    throw std::runtime_error(
        "unsupported order_profile.mode=" + mode + "; expected one of cash/spot/margin variants"
    );
}

enum class RequestLane { Default, Order, Poll };

inline std::string to_string(RequestLane lane) {
    switch (lane) {
    case RequestLane::Default:
        return "default";
    case RequestLane::Order:
        return "order";
    case RequestLane::Poll:
        return "poll";
    }
    return "default";
}

struct TransportResponse {
    int status{200};
    nlohmann::json payload{nlohmann::json::object()};
};

class TokenBucket {
  public:
    explicit TokenBucket(double rate_per_sec) : rate_per_sec_(rate_per_sec) {}
    void acquire() { ++acquire_count_; }
    [[nodiscard]] int acquire_count() const { return acquire_count_; }

  private:
    double rate_per_sec_{0.0};
    int acquire_count_{0};
};

class KabuRestClient {
  public:
    using RequestExecutor = std::function<TransportResponse(
        const std::string& method,
        const std::string& url,
        const nlohmann::json& json_body,
        const nlohmann::json& params,
        bool include_token,
        RequestLane lane
    )>;

    explicit KabuRestClient(
        std::string base_url,
        double rate_per_sec = 4.0,
        double order_rate_per_sec = -1.0,
        double poll_rate_per_sec = -1.0
    )
        : base_url_(std::move(base_url)),
          order_bucket_(order_rate_per_sec > 0 ? order_rate_per_sec : rate_per_sec),
          poll_bucket_(poll_rate_per_sec > 0 ? poll_rate_per_sec : rate_per_sec) {}

    void start() { running_ = true; }
    void stop() { running_ = false; }
    void set_request_executor(RequestExecutor executor) { executor_ = std::move(executor); }
    void set_token(std::string token) { token_ = std::move(token); }
    void set_password(std::string password) { password_ = std::move(password); }
    void set_debug_sendorder_log(bool enabled) { debug_sendorder_log_ = enabled; }
    [[nodiscard]] const std::string& token() const { return token_; }
    [[nodiscard]] const std::string& password() const { return password_; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool debug_sendorder_log() const { return debug_sendorder_log_; }

    [[nodiscard]] RequestLane bucket_for_request(
        const std::string& method,
        const std::string& path,
        std::optional<RequestLane> lane = std::nullopt
    ) const {
        if (lane.has_value() && *lane != RequestLane::Default) {
            return *lane;
        }
        static const std::unordered_set<std::string> mutation_paths{
            "/kabusapi/sendorder",
            "/kabusapi/sendorder/future",
            "/kabusapi/sendorder/option",
            "/kabusapi/cancelorder",
        };
        static const std::unordered_set<std::string> polling_paths{
            "/kabusapi/orders",
            "/kabusapi/positions",
        };
        if (mutation_paths.contains(path) || method == "POST" || method == "PUT") {
            return RequestLane::Order;
        }
        if (polling_paths.contains(path)) {
            return RequestLane::Poll;
        }
        return RequestLane::Order;
    }

    nlohmann::json request_json(
        const std::string& method,
        const std::string& path,
        const nlohmann::json& json_body = nlohmann::json::object(),
        const nlohmann::json& params = nlohmann::json::object(),
        bool include_token = true,
        std::optional<RequestLane> lane = std::nullopt
    ) {
        if (!executor_) {
            throw std::runtime_error("REST request executor has not been configured");
        }

        const auto request_lane = bucket_for_request(method, path, lane);
        if (request_lane == RequestLane::Poll) {
            poll_bucket_.acquire();
        } else {
            order_bucket_.acquire();
        }

        const std::string url = base_url_ + path;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const auto response = executor_(method, url, json_body, params, include_token, request_lane);
            if (response.status < 400) {
                return response.payload;
            }
            const bool should_retry =
                (response.status == 429 || response.status == 500 || response.status == 502 ||
                 response.status == 503 || response.status == 504) &&
                path != "/kabusapi/token" && path != "/kabusapi/register" &&
                path != "/kabusapi/sendorder" && path != "/kabusapi/sendorder/future" &&
                path != "/kabusapi/sendorder/option" && path != "/kabusapi/cancelorder";
            if (!should_retry || attempt >= 2) {
                throw KabuApiError("kabusapi request failed", response.status, response.payload);
            }
        }
        throw std::runtime_error("unreachable");
    }

    std::string get_token(const std::string& password) {
        const auto data = request_json(
            "POST",
            "/kabusapi/token",
            nlohmann::json{{"APIPassword", password}},
            nlohmann::json::object(),
            false,
            RequestLane::Order
        );
        const std::string token = parse_string(data.value("Token", nlohmann::json()), std::string());
        if (token.empty()) {
            throw KabuApiError("token response missing Token", 200, data);
        }
        token_ = token;
        password_ = password;
        return token_;
    }

    nlohmann::json register_symbols(const std::vector<nlohmann::json>& symbols) {
        return request_json(
            "PUT",
            "/kabusapi/register",
            nlohmann::json{{"Symbols", symbols}},
            nlohmann::json::object(),
            true,
            RequestLane::Order
        );
    }

    std::vector<nlohmann::json> get_positions(
        const std::optional<std::string>& symbol = std::nullopt,
        int product = 2,
        RequestLane lane = RequestLane::Poll
    );

    std::vector<nlohmann::json> get_orders(
        const std::optional<std::string>& order_id = std::nullopt,
        int product = 0,
        RequestLane lane = RequestLane::Poll
    );

    nlohmann::json cancel_order(const std::string& order_id);
    nlohmann::json send_entry_order(
        const std::string& symbol,
        int exchange,
        int side,
        int qty,
        double price,
        bool is_market,
        const kabu::config::OrderProfile& profile
    );
    nlohmann::json send_exit_order(
        const std::string& symbol,
        int exchange,
        int position_side,
        int qty,
        double price,
        bool is_market,
        const kabu::config::OrderProfile& profile
    );

    [[nodiscard]] int order_bucket_acquires() const { return order_bucket_.acquire_count(); }
    [[nodiscard]] int poll_bucket_acquires() const { return poll_bucket_.acquire_count(); }

  private:
    nlohmann::json sendorder_with_exchange_retry(const std::string& symbol, int exchange, const nlohmann::json& body);
    void log_sendorder_attempt(
        const std::string& symbol,
        int exchange,
        const nlohmann::json& body,
        int attempt_no,
        bool retry_exchange_fallback
    ) const;
    void log_sendorder_success(
        const std::string& symbol,
        int exchange,
        const nlohmann::json& body,
        int attempt_no,
        bool retry_exchange_fallback,
        const nlohmann::json& response
    ) const;
    void log_sendorder_error(
        const std::string& symbol,
        int exchange,
        const nlohmann::json& body,
        int attempt_no,
        bool retry_exchange_fallback,
        const KabuApiError& error
    ) const;
    void emit_sendorder_log(nlohmann::json event) const;
    [[nodiscard]] static nlohmann::json make_sendorder_log_base(
        const std::string& symbol,
        int exchange,
        const nlohmann::json& body,
        int attempt_no,
        bool retry_exchange_fallback
    );
    std::pair<std::vector<nlohmann::json>, std::vector<PositionLot>> build_close_positions(
        const std::string& symbol,
        int exchange,
        int position_side,
        int qty,
        bool strict_exchange,
        bool allow_mixed_exchanges
    );
    [[nodiscard]] static int resolve_margin_trade_type(int default_trade_type, const std::vector<PositionLot>& positions);

    std::string base_url_;
    std::string token_;
    std::string password_;
    TokenBucket order_bucket_;
    TokenBucket poll_bucket_;
    RequestExecutor executor_;
    bool running_{false};
    bool debug_sendorder_log_{false};
};

struct WebSocketStatusSnapshot {
    bool running{false};
    bool connected{false};
    bool stopped{true};
    int connect_count{0};
    int reconnect_count{0};
    std::int64_t last_connect_ts_ns{0};
    std::int64_t last_disconnect_ts_ns{0};
    std::int64_t last_message_ts_ns{0};
    std::int64_t last_board_ts_ns{0};
    std::int64_t last_trade_ts_ns{0};
    std::string last_error;

    [[nodiscard]] std::string status() const {
        if (connected) {
            return "connected";
        }
        if (running) {
            return "running";
        }
        return stopped ? "stopped" : "disconnected";
    }

    [[nodiscard]] nlohmann::json to_json() const {
        return {
            {"running", running},
            {"connected", connected},
            {"stopped", stopped},
            {"status", status()},
            {"connect_count", connect_count},
            {"reconnect_count", reconnect_count},
            {"last_connect_ts_ns", last_connect_ts_ns},
            {"last_disconnect_ts_ns", last_disconnect_ts_ns},
            {"last_message_ts_ns", last_message_ts_ns},
            {"last_board_ts_ns", last_board_ts_ns},
            {"last_trade_ts_ns", last_trade_ts_ns},
            {"last_error", last_error},
        };
    }
};

class KabuWebSocket {
  public:
    using BoardCallback = std::function<void(const BoardSnapshot&)>;
    using TradeCallback = std::function<void(const TradePrint&)>;
    using ReconnectCallback = std::function<void()>;

    KabuWebSocket(
        std::string url,
        BoardCallback on_board,
        TradeCallback on_trade,
        ReconnectCallback on_reconnect = {},
        std::string api_token = {}
    );
    ~KabuWebSocket();

    void run();
    void stop();
    void set_api_token(std::string api_token) { api_token_ = std::move(api_token); }
    [[nodiscard]] const std::string& api_token() const { return api_token_; }
    [[nodiscard]] bool running() const { return running_.load(); }
    [[nodiscard]] bool stopped() const { return !running_.load() && status_snapshot().stopped; }
    [[nodiscard]] bool connected() const { return status_snapshot().connected; }
    [[nodiscard]] std::string status() const { return status_snapshot().status(); }
    [[nodiscard]] WebSocketStatusSnapshot status_snapshot() const;
    [[nodiscard]] nlohmann::json snapshot_json() const { return status_snapshot().to_json(); }

    void simulate_board(const BoardSnapshot& snapshot);
    void simulate_trade(const TradePrint& trade);
    void simulate_reconnect() const;

  private:
    struct StreamState {
        std::optional<BoardSnapshot> snapshot;
        int volume{0};
        std::optional<double> last_trade_price;
    };

    [[nodiscard]] static std::int64_t system_now_ns();
    void note_connect(bool reconnect);
    void note_disconnect(const std::string& error_text = {});
    void note_message(std::int64_t ts_ns);
    void note_board(std::int64_t ts_ns);
    void note_trade(std::int64_t ts_ns);
    void reset_stream_state();
    void run_loop();

#ifdef _WIN32
    struct ParsedWebSocketUrl {
        bool secure{false};
        std::string host;
        std::uint16_t port{0};
        std::string target{"/"};
    };

    static std::wstring ws_utf8_to_utf16(const std::string& input);
    static std::string ws_utf16_to_utf8(const std::wstring& input);
    static std::string ws_last_error_message(const std::string& context, DWORD error_code = GetLastError());
    static ParsedWebSocketUrl parse_websocket_url(const std::string& url);
    void run_connected_loop(bool reconnect);
    std::string recv_message(HINTERNET websocket);
    void dispatch_message(const std::string& raw_message);
    void close_active_socket();
    void release_socket(HINTERNET websocket, bool mark_disconnect);
#else
    void close_active_socket() {}
#endif

    std::string url_;
    BoardCallback on_board_;
    TradeCallback on_trade_;
    ReconnectCallback on_reconnect_;
    std::string api_token_;
    std::atomic_bool running_{false};
    std::atomic_bool stop_requested_{false};
    mutable std::mutex state_mutex_;
    mutable std::mutex stream_mutex_;
    std::thread worker_;
    WebSocketStatusSnapshot status_;
    std::map<std::pair<std::string, int>, StreamState> streams_;
#ifdef _WIN32
    mutable std::mutex socket_mutex_;
    HINTERNET active_socket_{nullptr};
#endif
};

inline std::vector<nlohmann::json> KabuRestClient::get_positions(
    const std::optional<std::string>& symbol,
    int product,
    RequestLane lane
) {
    nlohmann::json params = {{"product", product}};
    if (symbol.has_value()) {
        params["symbol"] = *symbol;
    }
    const auto data = request_json("GET", "/kabusapi/positions", nlohmann::json::object(), params, true, lane);
    return data.is_array() ? data.get<std::vector<nlohmann::json>>() : std::vector<nlohmann::json>{data};
}

inline std::vector<nlohmann::json> KabuRestClient::get_orders(
    const std::optional<std::string>& order_id,
    int product,
    RequestLane lane
) {
    nlohmann::json params = {{"product", product}};
    if (order_id.has_value() && !order_id->empty()) {
        params["id"] = *order_id;
    }
    const auto data = request_json("GET", "/kabusapi/orders", nlohmann::json::object(), params, true, lane);
    return data.is_array() ? data.get<std::vector<nlohmann::json>>() : std::vector<nlohmann::json>{data};
}

inline nlohmann::json KabuRestClient::cancel_order(const std::string& order_id) {
    return request_json(
        "PUT",
        "/kabusapi/cancelorder",
        nlohmann::json{{"OrderId", order_id}, {"Password", password_}},
        nlohmann::json::object(),
        true,
        RequestLane::Order
    );
}

inline nlohmann::json KabuRestClient::send_entry_order(
    const std::string& symbol,
    int exchange,
    int side,
    int qty,
    double price,
    bool is_market,
    const kabu::config::OrderProfile& profile
) {
    const int route_exchange = is_margin_mode(profile.mode) ? normalize_margin_equity_exchange(exchange) : exchange;
    nlohmann::json body = {
        {"Password", password_},
        {"Symbol", symbol},
        {"Exchange", route_exchange},
        {"SecurityType", 1},
        {"Side", kabu_side(side)},
        {"Qty", qty},
        {"FrontOrderType", is_market ? profile.front_order_type_market : profile.front_order_type_limit},
        {"Price", is_market ? 0.0 : price},
        {"ExpireDay", 0},
        {"AccountType", profile.account_type},
    };
    if (is_margin_mode(profile.mode)) {
        body["CashMargin"] = 2;
        body["MarginTradeType"] = profile.margin_trade_type;
        body["DelivType"] = profile.margin_open_deliv_type;
        body["FundType"] = profile.margin_open_fund_type;
    } else {
        if (side < 0 && !profile.allow_short) {
            throw std::runtime_error("cash mode does not support opening short inventory");
        }
        body["CashMargin"] = 1;
        body["DelivType"] = side > 0 ? profile.cash_buy_deliv_type : profile.cash_sell_deliv_type;
        body["FundType"] = side > 0 ? profile.cash_buy_fund_type : profile.cash_sell_fund_type;
    }
    return sendorder_with_exchange_retry(symbol, route_exchange, body);
}

inline nlohmann::json KabuRestClient::send_exit_order(
    const std::string& symbol,
    int exchange,
    int position_side,
    int qty,
    double price,
    bool is_market,
    const kabu::config::OrderProfile& profile
) {
    const int route_exchange = is_margin_mode(profile.mode) ? normalize_margin_equity_exchange(exchange) : exchange;
    const int broker_side = -position_side;
    nlohmann::json body = {
        {"Password", password_},
        {"Symbol", symbol},
        {"Exchange", route_exchange},
        {"SecurityType", 1},
        {"Side", kabu_side(broker_side)},
        {"Qty", qty},
        {"FrontOrderType", is_market ? profile.front_order_type_market : profile.front_order_type_limit},
        {"Price", is_market ? 0.0 : price},
        {"ExpireDay", 0},
        {"AccountType", profile.account_type},
    };

    if (!is_margin_mode(profile.mode)) {
        body["CashMargin"] = 1;
        body["DelivType"] = broker_side > 0 ? profile.cash_buy_deliv_type : profile.cash_sell_deliv_type;
        body["FundType"] = broker_side > 0 ? profile.cash_buy_fund_type : profile.cash_sell_fund_type;
        return sendorder_with_exchange_retry(symbol, route_exchange, body);
    }

    const bool sor_route = route_exchange == 9;
    auto close_build = build_close_positions(symbol, route_exchange, position_side, qty, !sor_route, sor_route);
    body["CashMargin"] = 3;
    body["MarginTradeType"] = resolve_margin_trade_type(profile.margin_trade_type, close_build.second);
    body["DelivType"] = profile.margin_close_deliv_type;
    body["ClosePositions"] = close_build.first;
    return sendorder_with_exchange_retry(symbol, route_exchange, body);
}

inline nlohmann::json KabuRestClient::sendorder_with_exchange_retry(
    const std::string& symbol,
    int exchange,
    const nlohmann::json& body
) {
    log_sendorder_attempt(symbol, exchange, body, 1, false);
    try {
        const auto response = request_json(
            "POST",
            "/kabusapi/sendorder",
            body,
            nlohmann::json::object(),
            true,
            RequestLane::Order
        );
        log_sendorder_success(symbol, exchange, body, 1, false, response);
        return response;
    } catch (const KabuApiError& error) {
        log_sendorder_error(symbol, exchange, body, 1, false, error);
        const auto code = extract_error_code(error.payload());
        if (exchange != 1 || !code.has_value() || (*code != 100368 && *code != 100378)) {
            throw;
        }
        auto retry_body = body;
        retry_body["Exchange"] = 27;
        log_sendorder_attempt(symbol, 27, retry_body, 2, true);
        try {
            const auto response = request_json(
                "POST",
                "/kabusapi/sendorder",
                retry_body,
                nlohmann::json::object(),
                true,
                RequestLane::Order
            );
            log_sendorder_success(symbol, 27, retry_body, 2, true, response);
            return response;
        } catch (const KabuApiError& retry_error) {
            log_sendorder_error(symbol, 27, retry_body, 2, true, retry_error);
            throw;
        }
    }
}

inline void KabuRestClient::log_sendorder_attempt(
    const std::string& symbol,
    int exchange,
    const nlohmann::json& body,
    int attempt_no,
    bool retry_exchange_fallback
) const {
    auto event = make_sendorder_log_base(symbol, exchange, body, attempt_no, retry_exchange_fallback);
    event["event"] = "sendorder_attempt";
    emit_sendorder_log(std::move(event));
}

inline void KabuRestClient::log_sendorder_success(
    const std::string& symbol,
    int exchange,
    const nlohmann::json& body,
    int attempt_no,
    bool retry_exchange_fallback,
    const nlohmann::json& response
) const {
    auto event = make_sendorder_log_base(symbol, exchange, body, attempt_no, retry_exchange_fallback);
    event["event"] = "sendorder_result";
    event["result"] = "success";
    const std::string order_id = parse_string(
        response.contains("OrderId") ? response.at("OrderId") : response.value("ID", nlohmann::json()),
        std::string()
    );
    if (!order_id.empty()) {
        event["order_id"] = order_id;
    }
    emit_sendorder_log(std::move(event));
}

inline void KabuRestClient::log_sendorder_error(
    const std::string& symbol,
    int exchange,
    const nlohmann::json& body,
    int attempt_no,
    bool retry_exchange_fallback,
    const KabuApiError& error
) const {
    auto event = make_sendorder_log_base(symbol, exchange, body, attempt_no, retry_exchange_fallback);
    event["event"] = "sendorder_result";
    event["result"] = "error";
    event["http_status"] = error.status();
    if (const auto code = extract_error_code(error.payload()); code.has_value()) {
        event["error_code"] = *code;
    }
    if (const auto message = extract_error_message(error.payload()); message.has_value() && !message->empty()) {
        event["error_message"] = *message;
    }
    emit_sendorder_log(std::move(event));
}

inline void KabuRestClient::emit_sendorder_log(nlohmann::json event) const {
    if (!debug_sendorder_log_) {
        return;
    }
    std::cerr << event.dump() << '\n';
}

inline nlohmann::json KabuRestClient::make_sendorder_log_base(
    const std::string& symbol,
    int exchange,
    const nlohmann::json& body,
    int attempt_no,
    bool retry_exchange_fallback
) {
    nlohmann::json event = {
        {"symbol", parse_string(body.value("Symbol", nlohmann::json()), symbol)},
        {"exchange", parse_int(body.value("Exchange", nlohmann::json()), exchange)},
        {"qty", parse_int(body.value("Qty", nlohmann::json()))},
        {"side", parse_string(body.value("Side", nlohmann::json()))},
        {"cash_margin", parse_int(body.value("CashMargin", nlohmann::json()))},
        {"account_type", parse_int(body.value("AccountType", nlohmann::json()))},
        {"front_order_type", parse_int(body.value("FrontOrderType", nlohmann::json()))},
        {"price", parse_float(body.value("Price", nlohmann::json()))},
        {"attempt_no", attempt_no},
        {"retry_exchange_fallback", retry_exchange_fallback},
    };
    if (body.contains("MarginTradeType") && !body.at("MarginTradeType").is_null()) {
        event["margin_trade_type"] = parse_int(body.at("MarginTradeType"));
    }
    if (body.contains("DelivType") && !body.at("DelivType").is_null()) {
        event["deliv_type"] = parse_int(body.at("DelivType"));
    }
    if (body.contains("FundType") && !body.at("FundType").is_null()) {
        event["fund_type"] = parse_string(body.at("FundType"));
    }
    if (body.contains("ClosePositions") && body.at("ClosePositions").is_array()) {
        int total_qty = 0;
        for (const auto& position : body.at("ClosePositions")) {
            total_qty += parse_int(position.value("Qty", nlohmann::json()));
        }
        event["close_positions_count"] = static_cast<int>(body.at("ClosePositions").size());
        event["close_positions_total_qty"] = total_qty;
    }
    return event;
}

inline std::pair<std::vector<nlohmann::json>, std::vector<PositionLot>> KabuRestClient::build_close_positions(
    const std::string& symbol,
    int exchange,
    int position_side,
    int qty,
    bool strict_exchange,
    bool allow_mixed_exchanges
) {
    std::vector<PositionLot> positions;
    for (const auto& raw : get_positions(symbol, 0, RequestLane::Order)) {
        const auto lot = KabuAdapter::position_lot(raw);
        if (!lot.has_value()) {
            continue;
        }
        if (lot->side == position_side && lot->closable_qty > 0) {
            positions.push_back(*lot);
        }
    }

    std::vector<PositionLot> matched;
    std::copy_if(positions.begin(), positions.end(), std::back_inserter(matched), [&](const PositionLot& position) {
        return position.exchange == exchange;
    });

    std::vector<PositionLot> usable;
    if (strict_exchange && !matched.empty()) {
        usable = matched;
    } else if (strict_exchange) {
        usable.clear();
    } else if (allow_mixed_exchanges) {
        usable = positions;
    } else {
        std::set<int> exchanges;
        for (const auto& position : positions) {
            if (position.exchange > 0) {
                exchanges.insert(position.exchange);
            }
        }
        if (exchanges.size() > 1) {
            throw KabuApiError("ambiguous inventory exchange for close " + symbol, 0, nlohmann::json(positions));
        }
        usable = positions;
    }

    int remaining = qty;
    std::vector<nlohmann::json> close_positions;
    std::vector<PositionLot> selected_positions;
    for (const auto& position : usable) {
        if (remaining <= 0) {
            break;
        }
        const int take_qty = std::min(position.closable_qty, remaining);
        if (take_qty <= 0) {
            continue;
        }
        close_positions.push_back(nlohmann::json{{"HoldID", position.hold_id}, {"Qty", take_qty}});
        selected_positions.push_back(position);
        remaining -= take_qty;
    }

    if (remaining > 0) {
        throw KabuApiError(
            "not enough inventory to close " + symbol + " exchange=" + std::to_string(exchange) +
                " qty=" + std::to_string(qty),
            0,
            nlohmann::json(usable)
        );
    }
    return {close_positions, selected_positions};
}

inline int KabuRestClient::resolve_margin_trade_type(int default_trade_type, const std::vector<PositionLot>& positions) {
    std::set<int> trade_types;
    for (const auto& position : positions) {
        if (position.margin_trade_type > 0) {
            trade_types.insert(position.margin_trade_type);
        }
    }
    return trade_types.size() == 1 ? *trade_types.begin() : default_trade_type;
}

inline KabuWebSocket::KabuWebSocket(
    std::string url,
    BoardCallback on_board,
    TradeCallback on_trade,
    ReconnectCallback on_reconnect,
    std::string api_token
)
    : url_(std::move(url)),
      on_board_(std::move(on_board)),
      on_trade_(std::move(on_trade)),
      on_reconnect_(std::move(on_reconnect)),
      api_token_(std::move(api_token)) {}

inline KabuWebSocket::~KabuWebSocket() {
    stop();
}

inline WebSocketStatusSnapshot KabuWebSocket::status_snapshot() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return status_;
}

inline void KabuWebSocket::simulate_board(const BoardSnapshot& snapshot) {
    note_board(snapshot.ts_ns);
    if (on_board_) {
        on_board_(snapshot);
    }
}

inline void KabuWebSocket::simulate_trade(const TradePrint& trade) {
    note_trade(trade.ts_ns);
    if (on_trade_) {
        on_trade_(trade);
    }
}

inline void KabuWebSocket::simulate_reconnect() const {
    if (on_reconnect_) {
        on_reconnect_();
    }
}

inline std::int64_t KabuWebSocket::system_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()
           )
        .count();
}

inline void KabuWebSocket::note_connect(bool reconnect) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.connected = true;
    status_.stopped = false;
    status_.last_connect_ts_ns = system_now_ns();
    ++status_.connect_count;
    if (reconnect) {
        ++status_.reconnect_count;
    }
    status_.last_error.clear();
}

inline void KabuWebSocket::note_disconnect(const std::string& error_text) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.connected = false;
    status_.last_disconnect_ts_ns = system_now_ns();
    if (!error_text.empty()) {
        status_.last_error = error_text;
    }
}

inline void KabuWebSocket::note_message(std::int64_t ts_ns) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.last_message_ts_ns = ts_ns > 0 ? ts_ns : system_now_ns();
}

inline void KabuWebSocket::note_board(std::int64_t ts_ns) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.last_board_ts_ns = std::max(status_.last_board_ts_ns, ts_ns > 0 ? ts_ns : system_now_ns());
    status_.last_message_ts_ns = std::max(status_.last_message_ts_ns, status_.last_board_ts_ns);
}

inline void KabuWebSocket::note_trade(std::int64_t ts_ns) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    status_.last_trade_ts_ns = std::max(status_.last_trade_ts_ns, ts_ns > 0 ? ts_ns : system_now_ns());
    status_.last_message_ts_ns = std::max(status_.last_message_ts_ns, status_.last_trade_ts_ns);
}

inline void KabuWebSocket::reset_stream_state() {
    std::lock_guard<std::mutex> lock(stream_mutex_);
    streams_.clear();
}

inline void KabuWebSocket::run() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return;
    }
    stop_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.running = true;
        status_.stopped = false;
        status_.last_error.clear();
    }
    worker_ = std::thread([this]() { run_loop(); });
}

inline void KabuWebSocket::stop() {
    stop_requested_.store(true);
    close_active_socket();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.running = false;
        status_.connected = false;
        status_.stopped = true;
    }
}

inline void KabuWebSocket::run_loop() {
#ifdef _WIN32
    bool had_successful_connection = false;
    double retry_sleep_s = 1.0;
    while (!stop_requested_.load()) {
        try {
            run_connected_loop(had_successful_connection);
            had_successful_connection = true;
            retry_sleep_s = 1.0;
        } catch (const std::exception& error) {
            if (stop_requested_.load()) {
                break;
            }
            note_disconnect(error.what());
            std::this_thread::sleep_for(std::chrono::duration<double>(retry_sleep_s));
            retry_sleep_s = std::min(retry_sleep_s * 2.0, 5.0);
        }
    }
#else
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.last_error = "WebSocket client is only implemented for Windows";
    }
#endif
    running_.store(false);
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        status_.running = false;
        status_.connected = false;
        status_.stopped = true;
    }
}

#ifdef _WIN32
inline std::wstring KabuWebSocket::ws_utf8_to_utf16(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-8 text to UTF-16");
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size) <= 0) {
        throw std::runtime_error("failed to convert UTF-8 text to UTF-16");
    }
    return output;
}

inline std::string KabuWebSocket::ws_utf16_to_utf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-16 text to UTF-8");
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr) <=
        0) {
        throw std::runtime_error("failed to convert UTF-16 text to UTF-8");
    }
    return output;
}

inline std::string KabuWebSocket::ws_last_error_message(const std::string& context, DWORD error_code) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr
    );

    std::string detail;
    if (length > 0 && buffer != nullptr) {
        detail = ws_utf16_to_utf8(std::wstring(buffer, length));
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
            detail.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    std::ostringstream out;
    out << context << " (WinHTTP error " << error_code;
    if (!detail.empty()) {
        out << ": " << detail;
    }
    out << ")";
    return out.str();
}

inline KabuWebSocket::ParsedWebSocketUrl KabuWebSocket::parse_websocket_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::runtime_error("invalid WebSocket URL: missing scheme in " + url);
    }
    ParsedWebSocketUrl parsed;
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme == "ws") {
        parsed.secure = false;
        parsed.port = 80;
    } else if (scheme == "wss") {
        parsed.secure = true;
        parsed.port = 443;
    } else {
        throw std::runtime_error("unsupported WebSocket scheme: " + scheme);
    }

    const auto authority_start = scheme_end + 3;
    const auto path_start = url.find('/', authority_start);
    const std::string authority =
        path_start == std::string::npos ? url.substr(authority_start) : url.substr(authority_start, path_start - authority_start);
    parsed.target = path_start == std::string::npos ? "/" : url.substr(path_start);

    const auto colon_pos = authority.rfind(':');
    if (colon_pos != std::string::npos && authority.find(']') == std::string::npos) {
        parsed.host = authority.substr(0, colon_pos);
        parsed.port = static_cast<std::uint16_t>(std::stoi(authority.substr(colon_pos + 1)));
    } else {
        parsed.host = authority;
    }
    if (parsed.host.empty()) {
        throw std::runtime_error("invalid WebSocket URL: missing host in " + url);
    }
    return parsed;
}

inline void KabuWebSocket::run_connected_loop(bool reconnect) {
    const auto parsed = parse_websocket_url(url_);
    HINTERNET session = WinHttpOpen(
        L"kabu_micro_edge/0.1",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (session == nullptr) {
        throw std::runtime_error(ws_last_error_message("failed to create WinHTTP session"));
    }

    HINTERNET connection = WinHttpConnect(session, ws_utf8_to_utf16(parsed.host).c_str(), parsed.port, 0);
    if (connection == nullptr) {
        const auto message = ws_last_error_message("failed to connect to WebSocket host");
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        ws_utf8_to_utf16(parsed.target).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags
    );
    if (request == nullptr) {
        const auto message = ws_last_error_message("failed to create WebSocket request");
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    if (!WinHttpSetTimeouts(request, 5000, 5000, 15000, 15000)) {
        const auto message = ws_last_error_message("failed to configure WebSocket timeouts");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        const auto message = ws_last_error_message("failed to enable WebSocket upgrade");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (!api_token_.empty()) {
        headers += L"X-API-KEY: " + ws_utf8_to_utf16(api_token_) + L"\r\n";
    }
    if (!WinHttpSendRequest(
            request,
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0
        )) {
        const auto message = ws_last_error_message("failed to send WebSocket upgrade request");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        const auto message = ws_last_error_message("failed to receive WebSocket upgrade response");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    if (websocket == nullptr) {
        const auto message = ws_last_error_message("failed to complete WebSocket upgrade");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error(message);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        active_socket_ = websocket;
    }
    reset_stream_state();
    note_connect(reconnect);
    if (reconnect && on_reconnect_) {
        try {
            on_reconnect_();
        } catch (const std::exception& error) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            status_.last_error = error.what();
        } catch (...) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            status_.last_error = "WebSocket reconnect callback failed";
        }
    }

    try {
        while (!stop_requested_.load()) {
            const std::string raw_message = recv_message(websocket);
            dispatch_message(raw_message);
        }
    } catch (...) {
        release_socket(websocket, true);
        throw;
    }

    release_socket(websocket, true);
}

inline std::string KabuWebSocket::recv_message(HINTERNET websocket) {
    std::string message;
    std::vector<char> buffer(8192);

    for (;;) {
        DWORD bytes_read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE buffer_type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        const DWORD result = WinHttpWebSocketReceive(
            websocket,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            &buffer_type
        );
        if (result != ERROR_SUCCESS) {
            throw std::runtime_error(ws_last_error_message("failed to receive WebSocket frame", result));
        }
        if (buffer_type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            throw std::runtime_error("WebSocket peer initiated close");
        }
        if (bytes_read > 0) {
            message.append(buffer.data(), static_cast<std::size_t>(bytes_read));
        }
        const bool done = buffer_type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
                          buffer_type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        if (done) {
            return message;
        }
    }
}

inline void KabuWebSocket::dispatch_message(const std::string& raw_message) {
    const auto data = nlohmann::json::parse(raw_message, nullptr, false);
    if (!data.is_object()) {
        return;
    }

    const std::string symbol = parse_string(data.value("Symbol", nlohmann::json()), std::string());
    const int exchange = parse_int(data.value("Exchange", nlohmann::json()), 1);
    const auto key = std::make_pair(symbol, exchange);

    std::optional<BoardSnapshot> prev_snapshot;
    int prev_volume = 0;
    std::optional<double> last_trade_price;
    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        const auto it = streams_.find(key);
        if (it != streams_.end()) {
            prev_snapshot = it->second.snapshot;
            prev_volume = it->second.volume;
            last_trade_price = it->second.last_trade_price;
        }
    }

    const auto snapshot = KabuAdapter::board(data, prev_snapshot);
    if (!snapshot.has_value() || !snapshot->valid()) {
        return;
    }
    if (snapshot->out_of_order || snapshot->duplicate) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        auto& stream = streams_[key];
        stream.snapshot = snapshot;
        stream.volume = snapshot->volume;
    }

    note_message(system_now_ns());
    note_board(snapshot->ts_ns);
    if (on_board_) {
        on_board_(*snapshot);
    }

    if (!on_trade_) {
        return;
    }
    const auto trade = KabuAdapter::trade(data, prev_snapshot, prev_volume, last_trade_price);
    if (!trade.has_value()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(stream_mutex_);
        streams_[key].last_trade_price = trade->price;
    }
    note_trade(trade->ts_ns);
    on_trade_(*trade);
}

inline void KabuWebSocket::close_active_socket() {
    HINTERNET handle = nullptr;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        handle = active_socket_;
        active_socket_ = nullptr;
    }
    if (handle != nullptr) {
        WinHttpWebSocketClose(handle, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(handle);
    }
}

inline void KabuWebSocket::release_socket(HINTERNET websocket, bool mark_disconnect) {
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (active_socket_ == websocket) {
            active_socket_ = nullptr;
            should_close = true;
        }
    }
    if (mark_disconnect) {
        note_disconnect();
    }
    if (should_close && websocket != nullptr) {
        WinHttpCloseHandle(websocket);
    }
}
#endif

}  // namespace kabu::gateway
