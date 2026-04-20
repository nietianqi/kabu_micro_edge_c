#pragma once

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kabu::telemetry {

struct WeightedLatencyWindow {
    int samples{0};
    double p50_ms{0.0};
    double p90_ms{0.0};
    double p99_ms{0.0};
    double max_ms{0.0};
};

struct SymbolMetrics {
    std::string symbol;
    std::vector<double> warning_latencies_ms;
    std::vector<WeightedLatencyWindow> weighted_windows;
    std::map<std::string, int> source_counts;
    int entries{0};
    int cancels{0};
    int stale_cancels{0};
    std::map<std::string, int> cancel_reasons;

    [[nodiscard]] nlohmann::json est_latency() const {
        if (!weighted_windows.empty()) {
            int total = 0;
            double p50 = 0.0;
            double p90 = 0.0;
            double p99 = 0.0;
            double p99_worst = 0.0;
            double max_seen = 0.0;
            for (const auto& window : weighted_windows) {
                total += window.samples;
                p50 += window.p50_ms * window.samples;
                p90 += window.p90_ms * window.samples;
                p99 += window.p99_ms * window.samples;
                p99_worst = std::max(p99_worst, window.p99_ms);
                max_seen = std::max(max_seen, window.max_ms);
            }
            total = std::max(total, 1);
            return {
                {"samples", total},
                {"p50_ms", std::round((p50 / total) * 10.0) / 10.0},
                {"p90_ms", std::round((p90 / total) * 10.0) / 10.0},
                {"p99_ms", std::round((p99 / total) * 10.0) / 10.0},
                {"p99_worst_ms", std::round(p99_worst * 10.0) / 10.0},
                {"max_ms", std::round(max_seen * 10.0) / 10.0},
                {"method", "weighted_window"},
            };
        }
        return {{"samples", 0}, {"p50_ms", 0.0}, {"p90_ms", 0.0}, {"p99_ms", 0.0}, {"p99_worst_ms", 0.0}, {"max_ms", 0.0}, {"method", "none"}};
    }
};

struct RunMetrics {
    std::string label;
    std::string path;
    std::map<std::string, SymbolMetrics> symbols;
    int disconnects{0};
    int connects{0};
    double start_sod{0.0};
    double end_sod{0.0};
    bool has_start{false};
    bool has_end{false};
    int stale_trade_exits{0};
    int total_trade_rows{0};
    double stale_trade_exit_rate{0.0};

    SymbolMetrics& ensure_symbol(const std::string& symbol) {
        auto [it, _] = symbols.try_emplace(symbol, SymbolMetrics{symbol});
        return it->second;
    }

    [[nodiscard]] double duration_seconds() const {
        if (!has_start || !has_end) {
            return 0.0;
        }
        return end_sod >= start_sod ? end_sod - start_sod : end_sod + 24 * 3600.0 - start_sod;
    }

    [[nodiscard]] int total_entries() const {
        int value = 0;
        for (const auto& [_, symbol] : symbols) {
            value += symbol.entries;
        }
        return value;
    }

    [[nodiscard]] int total_cancels() const {
        int value = 0;
        for (const auto& [_, symbol] : symbols) {
            value += symbol.cancels;
        }
        return value;
    }

    [[nodiscard]] int total_stale_cancels() const {
        int value = 0;
        for (const auto& [_, symbol] : symbols) {
            value += symbol.stale_cancels;
        }
        return value;
    }
};

inline double parse_time_of_day_seconds(const std::string& line) {
    static const std::regex time_re(R"((\d{2}):(\d{2}):(\d{2})\.(\d{3}))");
    std::smatch match;
    if (!std::regex_search(line, match, time_re)) {
        return -1.0;
    }
    return std::stoi(match[1].str()) * 3600 + std::stoi(match[2].str()) * 60 + std::stoi(match[3].str()) +
           std::stoi(match[4].str()) / 1000.0;
}

inline RunMetrics analyze_log(const std::filesystem::path& path, const std::string& label = {}) {
    static const std::regex lat_warn_re(
        R"(market data latency (-?\d+(?:\.\d+)?)ms for (\S+)(?: \(source=([a-z_]+).*)?)"
    );
    static const std::regex lat_stats_re(
        R"(latency stats symbol=(\S+) samples=(\d+) p50=(-?\d+(?:\.\d+)?)ms p90=(-?\d+(?:\.\d+)?)ms p99=(-?\d+(?:\.\d+)?)ms max=(-?\d+(?:\.\d+)?)ms)"
    );
    static const std::regex entry_re(R"(entry order sent symbol=(\S+))");
    static const std::regex cancel_re(R"(cancel requested symbol=(\S+) .* reason=(\S+))");
    static const std::regex disconnect_re(R"(websocket disconnected)");
    static const std::regex connect_re(R"(websocket connected)");

    RunMetrics run;
    run.label = label.empty() ? path.stem().string() : label;
    run.path = path.string();
    std::ifstream handle(path);
    std::string line;
    while (std::getline(handle, line)) {
        const double ts = parse_time_of_day_seconds(line);
        if (ts >= 0.0) {
            if (!run.has_start) {
                run.start_sod = ts;
                run.has_start = true;
            }
            run.end_sod = ts;
            run.has_end = true;
        }

        if (std::regex_search(line, disconnect_re)) {
            ++run.disconnects;
        }
        if (std::regex_search(line, connect_re)) {
            ++run.connects;
        }

        std::smatch match;
        if (std::regex_search(line, match, lat_stats_re)) {
            auto& symbol = run.ensure_symbol(match[1].str());
            symbol.weighted_windows.push_back(WeightedLatencyWindow{
                std::stoi(match[2].str()),
                std::stod(match[3].str()),
                std::stod(match[4].str()),
                std::stod(match[5].str()),
                std::stod(match[6].str()),
            });
            continue;
        }
        if (std::regex_search(line, match, lat_warn_re)) {
            auto& symbol = run.ensure_symbol(match[2].str());
            symbol.warning_latencies_ms.push_back(std::stod(match[1].str()));
            if (match[3].matched) {
                ++symbol.source_counts[match[3].str()];
            }
            continue;
        }
        if (std::regex_search(line, match, entry_re)) {
            ++run.ensure_symbol(match[1].str()).entries;
            continue;
        }
        if (std::regex_search(line, match, cancel_re)) {
            auto& symbol = run.ensure_symbol(match[1].str());
            ++symbol.cancels;
            ++symbol.cancel_reasons[match[2].str()];
            if (match[2].str().find("stale_quote") != std::string::npos) {
                ++symbol.stale_cancels;
            }
        }
    }
    return run;
}

inline void attach_trade_stale_exit_rate(RunMetrics& run, const std::filesystem::path& trades_csv) {
    if (!std::filesystem::exists(trades_csv)) {
        return;
    }
    std::ifstream handle(trades_csv);
    std::string line;
    bool first = true;
    while (std::getline(handle, line)) {
        if (first) {
            first = false;
            continue;
        }
        if (line.empty()) {
            continue;
        }
        ++run.total_trade_rows;
        if (line.find("abnormal_stale_quote") != std::string::npos) {
            ++run.stale_trade_exits;
        }
    }
    if (run.total_trade_rows > 0) {
        run.stale_trade_exit_rate = 100.0 * run.stale_trade_exits / run.total_trade_rows;
    }
}

}  // namespace kabu::telemetry
