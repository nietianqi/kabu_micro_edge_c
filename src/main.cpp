#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/app.hpp"
#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/strategy.hpp"

int main(int argc, char** argv) {
    std::string config_path;
    bool print_status = false;
    bool print_register_payload = false;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (arg == "--print-status") {
            print_status = true;
        } else if (arg == "--print-register-payload") {
            print_register_payload = true;
        }
    }

    const auto config = config_path.empty() ? kabu::config::load_config() : kabu::config::load_config(config_path);
    kabu::app::MicroEdgeApp app(config);
    app.set_running(true);
    std::vector<std::shared_ptr<kabu::strategy::MicroEdgeStrategy>> strategies;
    strategies.reserve(config.symbols.size());
    for (const auto& symbol : config.symbols) {
        auto strategy = std::make_shared<kabu::strategy::MicroEdgeStrategy>(
            symbol,
            config.strategy,
            config.order_profile,
            config.dry_run,
            nullptr,
            app.make_account_entry_guard(),
            config.event_queue_maxsize
        );
        strategy->start();
        app.register_strategy(symbol, strategy);
        strategies.push_back(strategy);
    }

    nlohmann::json payload = {
        {"dry_run", config.dry_run},
        {"symbol_count", config.symbols.size()},
        {"primary_symbol", config.symbol().symbol},
        {"kill_switch_path", config.kill_switch_path},
    };
    if (print_register_payload) {
        payload["register_payload"] = app.build_register_payload();
    }
    if (print_status) {
        payload["app"] = app.status_snapshot();
    }

    std::cout << std::setw(2) << payload << "\n";
    return 0;
}
