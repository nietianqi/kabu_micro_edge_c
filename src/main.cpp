#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <filesystem>
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

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    bool print_status = false;
    bool print_register_payload = false;
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
            }
        }
        const auto config = config_path.empty() ? kabu::config::load_config() : kabu::config::load_config(config_path);
        kabu::app::MicroEdgeApp app(config);
        app.set_running(true);
        if (!config.dry_run) {
            app.set_rest_request_executor(kabu::gateway::make_live_rest_request_executor(app.rest()));
            app.rest().start();
        }

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

        if (print_status || print_register_payload) {
            std::cout << std::setw(2)
                      << build_payload(config, app, print_status, print_register_payload)
                      << "\n";
            if (!run_seconds.has_value()) {
                return 0;
            }
        }

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
            std::cerr
                << "REST startup completed. Market-data WebSocket wiring is still a stub, so this runtime only drives timers.\n";
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

        for (const auto& strategy : strategies) {
            strategy->stop();
        }
        app.rest().stop();
        app.set_running(false);

        std::cout << std::setw(2) << build_payload(config, app, true, false) << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "kabu_micro_edge failed: " << error.what() << "\n";
        return 1;
    }
}
