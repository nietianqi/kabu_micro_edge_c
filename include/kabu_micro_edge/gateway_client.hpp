#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "kabu_micro_edge/config.hpp"
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
    [[nodiscard]] const std::string& token() const { return token_; }
    [[nodiscard]] const std::string& password() const { return password_; }
    [[nodiscard]] bool running() const { return running_; }

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
        const std::string token = data.value("Token", std::string());
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
    ) {
        nlohmann::json params = {{"product", product}};
        if (symbol.has_value()) {
            params["symbol"] = *symbol;
        }
        const auto data = request_json("GET", "/kabusapi/positions", nlohmann::json::object(), params, true, lane);
        return data.is_array() ? data.get<std::vector<nlohmann::json>>() : std::vector<nlohmann::json>{data};
    }

    std::vector<nlohmann::json> get_orders(
        const std::optional<std::string>& order_id = std::nullopt,
        int product = 0,
        RequestLane lane = RequestLane::Poll
    ) {
        nlohmann::json params = {{"product", product}};
        if (order_id.has_value() && !order_id->empty()) {
            params["id"] = *order_id;
        }
        const auto data = request_json("GET", "/kabusapi/orders", nlohmann::json::object(), params, true, lane);
        return data.is_array() ? data.get<std::vector<nlohmann::json>>() : std::vector<nlohmann::json>{data};
    }

    nlohmann::json send_entry_order(
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
            body["CashMargin"] = 1;
            body["DelivType"] = side > 0 ? profile.cash_buy_deliv_type : profile.cash_sell_deliv_type;
            body["FundType"] = side > 0 ? profile.cash_buy_fund_type : profile.cash_sell_fund_type;
        }
        return request_json("POST", "/kabusapi/sendorder", body, nlohmann::json::object(), true, RequestLane::Order);
    }

    [[nodiscard]] int order_bucket_acquires() const { return order_bucket_.acquire_count(); }
    [[nodiscard]] int poll_bucket_acquires() const { return poll_bucket_.acquire_count(); }

  private:
    std::string base_url_;
    std::string token_;
    std::string password_;
    TokenBucket order_bucket_;
    TokenBucket poll_bucket_;
    RequestExecutor executor_;
    bool running_{false};
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
    )
        : url_(std::move(url)),
          on_board_(std::move(on_board)),
          on_trade_(std::move(on_trade)),
          on_reconnect_(std::move(on_reconnect)),
          api_token_(std::move(api_token)) {}

    void run() {
        stopped_ = false;
        running_ = true;
    }
    void stop() {
        stopped_ = true;
        running_ = false;
    }
    void set_api_token(std::string api_token) { api_token_ = std::move(api_token); }
    [[nodiscard]] const std::string& api_token() const { return api_token_; }
    [[nodiscard]] bool running() const { return running_; }
    [[nodiscard]] bool stopped() const { return stopped_; }
    [[nodiscard]] std::string status() const { return running_ ? "running" : "stopped"; }

    void simulate_board(const BoardSnapshot& snapshot) const {
        if (on_board_) {
            on_board_(snapshot);
        }
    }

    void simulate_trade(const TradePrint& trade) const {
        if (on_trade_) {
            on_trade_(trade);
        }
    }

    void simulate_reconnect() const {
        if (on_reconnect_) {
            on_reconnect_();
        }
    }

  private:
    std::string url_;
    BoardCallback on_board_;
    TradeCallback on_trade_;
    ReconnectCallback on_reconnect_;
    std::string api_token_;
    bool running_{false};
    bool stopped_{false};
};

}  // namespace kabu::gateway
