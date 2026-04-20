#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/gateway.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace kabu::replay {

struct ReplayEvent {
    std::string kind;
    nlohmann::json payload;
};

class ReplayRunner {
  public:
    explicit ReplayRunner(strategy::MicroEdgeStrategy& strategy) : strategy_(strategy) {}

    [[nodiscard]] std::vector<nlohmann::json> run(const std::vector<ReplayEvent>& events) {
        std::vector<nlohmann::json> snapshots;
        for (const auto& event : events) {
            if (event.kind == "board") {
                strategy_.process_board(event.payload.get<gateway::BoardSnapshot>());
            } else if (event.kind == "trade") {
                strategy_.process_trade(event.payload.get<gateway::TradePrint>());
            } else if (event.kind == "timer") {
                strategy_.on_timer(event.payload.get<std::int64_t>());
            } else {
                throw std::runtime_error("unsupported replay event kind=" + event.kind);
            }
            snapshots.push_back(strategy_.status());
        }
        return snapshots;
    }

  private:
    strategy::MicroEdgeStrategy& strategy_;
};

}  // namespace kabu::replay
