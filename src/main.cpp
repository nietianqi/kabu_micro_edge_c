#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/app.hpp"
#include "kabu_micro_edge/config.hpp"
#include "kabu_micro_edge/live_rest_executor.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace {

std::atomic_bool g_keep_running{true};

void handle_signal(int) {
    g_keep_running.store(false);
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch()
           )
        .count();
}

nlohmann::json build_payload(
    const kabu::config::AppConfig& config,
    kabu::app::MicroEdgeApp& app,
    bool include_status,
    bool include_register_payload
) {
    nlohmann::json payload = {
        {"dry_run", config.dry_run},
        {"symbol_count", config.symbols.size()},
        {"primary_symbol", config.symbol().symbol},
        {"kill_switch_path", config.kill_switch_path},
    };

    if (include_register_payload) {
        payload["register_payload"] = app.build_register_payload();
    }
    if (include_status) {
        payload["app"] = app.status_snapshot();
    }
    return payload;
}

bool is_placeholder_password(const std::string& value) {
    return value.empty() || value == "YOUR_KABU_API_PASSWORD";
}

void configure_live_execution(
    kabu::app::MicroEdgeApp& app,
    const kabu::config::AppConfig& config,
    const kabu::config::SymbolConfig& symbol,
    kabu::strategy::MicroEdgeStrategy& strategy
) {
    strategy.execution().set_live_order_callbacks(
        [&app, symbol, &config](int side, int qty, double price, bool is_market) {
            const auto response =
                app.rest().send_entry_order(symbol.symbol, symbol.exchange, side, qty, price, is_market, config.order_profile);
            return kabu::gateway::parse_string(
                response.contains("OrderId") ? response.at("OrderId") : response.value("ID", nlohmann::json()),
                std::string()
            );
        },
        [&app, symbol, &config](int position_side, int qty, double price, bool is_market) {
            const auto response = app.rest().send_exit_order(
                symbol.symbol,
                symbol.exchange,
                position_side,
                qty,
                price,
                is_market,
                config.order_profile
            );
            return kabu::gateway::parse_string(
                response.contains("OrderId") ? response.at("OrderId") : response.value("ID", nlohmann::json()),
                std::string()
            );
        },
        [&app](const std::string& order_id) { app.rest().cancel_order(order_id); }
    );
}

void reconcile_startup_state(kabu::app::MicroEdgeApp& app) {
    const auto positions = app.with_authorization_retry(
        [&]() { return app.rest().get_positions(std::nullopt, 0, kabu::gateway::RequestLane::Poll); }
    );
    const auto orders = app.with_authorization_retry(
        [&]() { return app.rest().get_orders(std::nullopt, 0, kabu::gateway::RequestLane::Poll); }
    );
    std::map<std::string, kabu::gateway::OrderSnapshot> order_snapshots;
    for (const auto& raw : orders) {
        const auto snapshot = kabu::gateway::KabuAdapter::order_snapshot(raw);
        if (snapshot.has_value() && !snapshot->order_id.empty()) {
            order_snapshots[snapshot->order_id] = *snapshot;
        }
    }

    const auto timestamp_ns = now_ns();
    for (auto& [_, runtime] : app.strategy_runtimes()) {
        runtime.strategy->reconcile_with_prefetched(positions, order_snapshots, timestamp_ns);
        runtime.next_reconcile_at_monotonic = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() +
                                              app.build_reconcile_plan(*runtime.strategy).sleep_s;
    }
    app.note_reconcile_success();
}

void run_reconcile_cycle(kabu::app::MicroEdgeApp& app) {
    auto& runtimes = app.strategy_runtimes();
    const auto loop_now = std::chrono::steady_clock::now();
    const double loop_now_s = std::chrono::duration<double>(loop_now.time_since_epoch()).count();

    std::vector<std::pair<std::pair<std::string, int>, kabu::app::ReconcilePlan>> due;
    for (auto& [key, runtime] : runtimes) {
        const auto plan = app.build_reconcile_plan(*runtime.strategy);
        if (loop_now_s >= runtime.next_reconcile_at_monotonic || app.should_fast_track_reconcile(plan, *runtime.strategy)) {
            due.push_back({key, plan});
        }
    }
    if (due.empty()) {
        return;
    }

    std::vector<nlohmann::json> positions;
    try {
        positions = app.with_authorization_retry(
            [&]() { return app.rest().get_positions(std::nullopt, 0, kabu::gateway::RequestLane::Poll); }
        );
    } catch (const std::exception& error) {
        app.note_reconcile_failure(error.what());
        return;
    }

    std::set<std::string> order_ids;
    for (const auto& [key, plan] : due) {
        (void)key;
        order_ids.insert(plan.order_ids.begin(), plan.order_ids.end());
    }

    std::map<std::string, kabu::gateway::OrderSnapshot> order_snapshots;
    try {
        const auto all_orders = app.with_authorization_retry(
            [&]() { return app.rest().get_orders(std::nullopt, 0, kabu::gateway::RequestLane::Poll); }
        );
        for (const auto& raw : all_orders) {
            const auto snapshot = kabu::gateway::KabuAdapter::order_snapshot(raw);
            if (!snapshot.has_value() || snapshot->order_id.empty()) {
                continue;
            }
            if (order_ids.empty() || order_ids.contains(snapshot->order_id) ||
                (!snapshot->symbol.empty() &&
                 runtimes.contains({snapshot->symbol, snapshot->exchange}))) {
                order_snapshots[snapshot->order_id] = *snapshot;
            }
        }
    } catch (const std::exception& error) {
        app.note_reconcile_failure(error.what());
        return;
    }

    const auto timestamp_ns = now_ns();
    for (const auto& [key, plan] : due) {
        auto it = runtimes.find(key);
        if (it == runtimes.end()) {
            continue;
        }
        it->second.strategy->reconcile_with_prefetched(positions, order_snapshots, timestamp_ns);
        it->second.next_reconcile_at_monotonic = loop_now_s + plan.sleep_s;
    }
    app.note_reconcile_success();
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    bool print_status = false;
    bool print_register_payload = false;
    bool health_check = false;
    std::optional<double> run_seconds;

    try {
        for (int index = 1; index < argc; ++index) {
            const std::string arg = argv[index];
            if (arg == "--config" && index + 1 < argc) {
                config_path = argv[++index];
            } else if (arg == "--print-status") {
                print_status = true;
            } else if (arg == "--print-register-payload") {
                print_register_payload = true;
            } else if (arg == "--run-seconds" && index + 1 < argc) {
                run_seconds = std::stod(argv[++index]);
                if (*run_seconds <= 0.0) {
                    throw std::runtime_error("--run-seconds must be > 0");
                }
            } else if (arg == "--health-check") {
                health_check = true;
            }
        }

        if (health_check && !run_seconds.has_value()) {
            run_seconds = 30.0;
        }

        const auto config = config_path.empty() ? kabu::config::load_config() : kabu::config::load_config(config_path);
        const bool strategy_dry_run = config.dry_run || health_check;
        kabu::app::MicroEdgeApp app(config);
        app.set_running(true);

        if (!config.dry_run) {
            app.set_rest_request_executor(kabu::gateway::make_live_rest_request_executor(app.rest()));
            app.rest().start();
        } else {
            app.set_websocket_status_provider([] {
                return nlohmann::json{{"status", "dry_run"}, {"running", false}, {"connected", false}, {"stopped", true}};
            });
        }

        std::vector<std::shared_ptr<kabu::TradeJournal>> journals;
        std::vector<std::shared_ptr<kabu::strategy::MicroEdgeStrategy>> strategies;
        journals.reserve(config.symbols.size());
        strategies.reserve(config.symbols.size());
        for (const auto& symbol : config.symbols) {
            auto journal = std::make_shared<kabu::TradeJournal>(
                config.journal_path_for(symbol),
                config.markout_seconds,
                std::vector<double>{0.1, 0.5, 1.0, 3.0, static_cast<double>(config.markout_seconds)},
                std::vector<double>{0.2, 0.5, 1.0}
            );
            journal->open();
            journals.push_back(journal);

            auto strategy = std::make_shared<kabu::strategy::MicroEdgeStrategy>(
                symbol,
                config.strategy,
                config.order_profile,
                strategy_dry_run,
                journal,
                app.make_account_entry_guard(),
                config.event_queue_maxsize
            );
            if (!config.dry_run && !health_check) {
                configure_live_execution(app, config, symbol, *strategy);
            }
            strategy->start();
            app.register_strategy(symbol, strategy);
            strategies.push_back(strategy);
        }

        if (print_status || print_register_payload) {
            std::cout << std::setw(2)
                      << build_payload(config, app, print_status, print_register_payload)
                      << "\n";
            if (!run_seconds.has_value()) {
                return 0;
            }
        }

        std::unique_ptr<kabu::gateway::KabuWebSocket> websocket;
        bool received_market_data = false;
        std::string last_ws_error;

        if (!config.dry_run) {
            if (is_placeholder_password(config.api_password)) {
                std::cerr << "Live mode requires config.api_password to be set to a real kabus API password.\n";
                return 2;
            }

            if (!config.kill_switch_path.empty()) {
                const std::filesystem::path kill_switch_path(config.kill_switch_path);
                if (kill_switch_path.has_parent_path()) {
                    std::filesystem::create_directories(kill_switch_path.parent_path());
                }
            }

            app.startup_with_retry([](double delay_s) {
                std::this_thread::sleep_for(std::chrono::duration<double>(delay_s));
            });
            reconcile_startup_state(app);

            websocket = std::make_unique<kabu::gateway::KabuWebSocket>(
                config.ws_url,
                [&](const kabu::gateway::BoardSnapshot& snapshot) {
                    received_market_data = true;
                    app.on_board(snapshot);
                },
                [&](const kabu::gateway::TradePrint& trade) {
                    received_market_data = true;
                    app.on_trade(trade);
                },
                [&]() {
                    app.reregister_symbols();
                    reconcile_startup_state(app);
                },
                app.rest().token()
            );
            app.set_websocket_status_provider([&websocket]() {
                return websocket ? websocket->snapshot_json()
                                 : nlohmann::json{{"status", "stopped"}, {"running", false}, {"connected", false}, {"stopped", true}};
            });
            websocket->run();
            if (health_check) {
                std::cout << "Running live health check in observe-only mode. REST/WS are real; strategy orders stay paper-only.\n";
            }
        } else {
            std::cerr << "Running in local dry-run mode. No REST or WebSocket connection will be opened.\n";
        }

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        const auto start = std::chrono::steady_clock::now();
        auto next_tick = start;
        auto next_status = start + std::chrono::seconds(config.status_interval_s);

        while (g_keep_running.load()) {
            const auto loop_now = std::chrono::steady_clock::now();
            const auto timestamp_ns = now_ns();

            for (const auto& strategy : strategies) {
                strategy->on_timer(timestamp_ns);
            }
            app.poll_kill_switch();

            if (!config.dry_run) {
                run_reconcile_cycle(app);
                if (websocket) {
                    const auto ws_status = websocket->status_snapshot();
                    if (!ws_status.last_error.empty() && ws_status.last_error != last_ws_error) {
                        last_ws_error = ws_status.last_error;
                        app.note_websocket_error(last_ws_error);
                    }
                }
            }

            if (health_check && received_market_data) {
                break;
            }

            if (loop_now >= next_status) {
                std::cout << app.status_snapshot().dump() << "\n";
                next_status = loop_now + std::chrono::seconds(config.status_interval_s);
            }

            if (run_seconds.has_value()) {
                const auto elapsed_s = std::chrono::duration<double>(loop_now - start).count();
                if (elapsed_s >= *run_seconds) {
                    break;
                }
            }

            next_tick += std::chrono::milliseconds(config.timer_interval_ms);
            const auto sleep_until = next_tick > std::chrono::steady_clock::now()
                                         ? next_tick
                                         : std::chrono::steady_clock::now() + std::chrono::milliseconds(config.timer_interval_ms);
            std::this_thread::sleep_until(sleep_until);
        }

        if (websocket) {
            websocket->stop();
        }
        for (const auto& strategy : strategies) {
            strategy->stop();
        }
        for (const auto& journal : journals) {
            journal->close();
        }
        app.rest().stop();
        app.set_running(false);

        std::cout << std::setw(2) << build_payload(config, app, true, false) << "\n";
        if (health_check && !received_market_data) {
            std::cerr << "Health check failed: connected startup completed but no market data was observed.\n";
            return 3;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "kabu_micro_edge failed: " << error.what() << "\n";
        return 1;
    }
}
