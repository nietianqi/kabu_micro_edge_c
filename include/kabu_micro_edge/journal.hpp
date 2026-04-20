#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/common/time.hpp"
#include "kabu_micro_edge/execution.hpp"
#include "kabu_micro_edge/signals.hpp"

namespace kabu {

class TradeJournal {
  public:
    explicit TradeJournal(
        std::filesystem::path trade_path,
        int markout_seconds = 30,
        std::vector<double> markout_horizons_seconds = {},
        std::vector<double> entry_markout_horizons_seconds = {}
    )
        : trade_path_(std::move(trade_path)),
          markout_path_(with_replaced_extension(trade_path_, ".markout.csv")),
          entry_markout_path_(with_replaced_extension(trade_path_, ".entry_markout.csv")),
          default_markout_seconds_(static_cast<double>(markout_seconds)),
          markout_horizons_(resolve_horizons(markout_horizons_seconds, static_cast<double>(markout_seconds))),
          entry_markout_horizons_(resolve_horizons(entry_markout_horizons_seconds, 0.0, {0.2, 0.5, 1.0})) {}

    void open() {
        if (!trade_path_.parent_path().empty()) {
            std::filesystem::create_directories(trade_path_.parent_path());
        }
        open_file(trade_file_, trade_path_, trade_header());
        if (!markout_horizons_.empty()) {
            if (!markout_path_.parent_path().empty()) {
                std::filesystem::create_directories(markout_path_.parent_path());
            }
            open_file(markout_file_, markout_path_, markout_header());
        }
        if (!entry_markout_horizons_.empty()) {
            if (!entry_markout_path_.parent_path().empty()) {
                std::filesystem::create_directories(entry_markout_path_.parent_path());
            }
            open_file(entry_markout_file_, entry_markout_path_, entry_markout_header());
        }
    }

    void close() {
        flush_pending_markouts();
        flush_pending_entry_markouts();
        if (trade_file_.is_open()) {
            trade_file_.flush();
            trade_file_.close();
        }
        if (markout_file_.is_open()) {
            markout_file_.flush();
            markout_file_.close();
        }
        if (entry_markout_file_.is_open()) {
            entry_markout_file_.flush();
            entry_markout_file_.close();
        }
    }

    void log_trade(const execution::RoundTrip& trade, const std::optional<signals::SignalPacket>& signal = std::nullopt) {
        if (!trade_file_.is_open()) {
            return;
        }
        const double hold_ms = std::max(0.0, static_cast<double>(trade.exit_ts_ns - trade.entry_ts_ns) / 1'000'000.0);
        std::vector<std::string> row{
            common::format_jst_iso(trade.exit_ts_ns),
            trade.symbol,
            std::to_string(trade.side),
            std::to_string(trade.qty),
            format_fixed(trade.entry_price, 4),
            format_fixed(trade.exit_price, 4),
            format_fixed(trade.realized_pnl, 2),
            format_fixed(hold_ms, 1),
            trade.exit_reason,
            trade.entry_mode,
            std::to_string(trade.entry_score),
            trade.fill_reason,
            std::to_string(trade.queue_ahead_qty),
        };
        if (signal.has_value()) {
            row.push_back(format_fixed(signal->obi_z, 4));
            row.push_back(format_fixed(signal->lob_ofi_z, 4));
            row.push_back(format_fixed(signal->tape_ofi_z, 4));
            row.push_back(format_fixed(signal->micro_momentum_z, 4));
            row.push_back(format_fixed(signal->microprice_tilt_z, 4));
            row.push_back(format_fixed(signal->composite, 4));
        } else {
            row.insert(row.end(), 6, "");
        }
        write_row(trade_file_, row);
        ++trade_count_;
    }

    void schedule_markout(const execution::RoundTrip& trade, const std::vector<double>& mid_ref) {
        if (!markout_file_.is_open()) {
            return;
        }
        for (double horizon : markout_horizons_) {
            pending_markouts_.push_back(PostExitMarkoutTask{trade, horizon, mid_ref.empty() ? 0.0 : mid_ref.front()});
        }
    }

    void schedule_entry_markout(
        const std::string& symbol,
        int side,
        int qty,
        double entry_price,
        std::int64_t entry_ts_ns,
        const std::string& entry_mode,
        int entry_score,
        const std::string& fill_reason,
        int queue_ahead_qty,
        const std::vector<double>& mid_ref
    ) {
        if (!entry_markout_file_.is_open() || qty <= 0 || entry_price <= 0) {
            return;
        }
        for (double horizon : entry_markout_horizons_) {
            pending_entry_markouts_.push_back(EntryMarkoutTask{
                symbol,
                side,
                qty,
                entry_price,
                entry_ts_ns,
                entry_mode,
                entry_score,
                fill_reason,
                queue_ahead_qty,
                horizon,
                mid_ref.empty() ? 0.0 : mid_ref.front(),
            });
        }
    }

    [[nodiscard]] nlohmann::json snapshot() const {
        nlohmann::json post_exit_horizons = nlohmann::json::object();
        for (const auto& [key, value] : post_exit_stats_) {
            post_exit_horizons[key] = {
                {"count", value.count},
                {"avg_markout_pnl", value.count == 0 ? 0.0 : value.total_markout_pnl / value.count},
            };
        }
        nlohmann::json entry_horizons = nlohmann::json::object();
        for (const auto& [key, value] : entry_stats_) {
            entry_horizons[key] = {
                {"count", value.count},
                {"avg_markout_pnl", value.count == 0 ? 0.0 : value.total_markout_pnl / value.count},
            };
        }

        return {
            {"trade_count", trade_count_},
            {"pending_markouts", 0},
            {"pending_post_exit_markouts", 0},
            {"pending_entry_markouts", 0},
            {"markout_horizons", post_exit_horizons},
            {"post_exit_horizons", post_exit_horizons},
            {"entry_horizons", entry_horizons},
        };
    }

    [[nodiscard]] const std::filesystem::path& trade_path() const { return trade_path_; }
    [[nodiscard]] const std::filesystem::path& markout_path() const { return markout_path_; }
    [[nodiscard]] const std::filesystem::path& entry_markout_path() const { return entry_markout_path_; }

  private:
    struct HorizonStats {
        int count{0};
        double total_markout_pnl{0.0};
    };

    struct PostExitMarkoutTask {
        execution::RoundTrip trade;
        double horizon{0.0};
        double markout_mid{0.0};
    };

    struct EntryMarkoutTask {
        std::string symbol;
        int side{0};
        int qty{0};
        double entry_price{0.0};
        std::int64_t entry_ts_ns{0};
        std::string entry_mode;
        int entry_score{0};
        std::string fill_reason;
        int queue_ahead_qty{0};
        double horizon{0.0};
        double markout_mid{0.0};
    };

    static std::vector<double> resolve_horizons(
        const std::vector<double>& explicit_horizons,
        double base_markout_seconds,
        const std::vector<double>& fallback = {}
    ) {
        std::vector<double> values = explicit_horizons;
        if (values.empty()) {
            values = base_markout_seconds > 0 ? std::vector<double>{base_markout_seconds} : fallback;
        }
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());
        return values;
    }

    static std::filesystem::path with_replaced_extension(const std::filesystem::path& path, const std::string& extension) {
        auto copy = path;
        copy.replace_extension(extension);
        return copy;
    }

    static void open_file(std::ofstream& file, const std::filesystem::path& path, const std::vector<std::string>& header) {
        const bool write_header = !std::filesystem::exists(path) || std::filesystem::file_size(path) == 0;
        file.open(path, std::ios::app);
        if (write_header) {
            write_row(file, header);
        }
    }

    static void write_row(std::ofstream& file, const std::vector<std::string>& values) {
        for (std::size_t index = 0; index < values.size(); ++index) {
            if (index > 0) {
                file << ",";
            }
            file << values[index];
        }
        file << "\n";
        file.flush();
    }

    static std::string format_fixed(double value, int precision) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(precision) << value;
        return out.str();
    }

    static std::vector<std::string> trade_header() {
        return {
            "ts_jst", "symbol", "side", "qty", "entry_price", "exit_price", "realized_pnl", "hold_ms",
            "exit_reason", "entry_mode", "entry_score", "fill_reason", "queue_ahead_qty", "obi_z", "lob_ofi_z",
            "tape_ofi_z", "micro_momentum_z", "microprice_tilt_z", "composite"
        };
    }

    static std::vector<std::string> markout_header() {
        return {
            "ts_jst", "symbol", "side", "qty", "entry_price", "exit_price", "realized_pnl", "exit_reason",
            "entry_mode", "entry_score", "fill_reason", "queue_ahead_qty", "markout_seconds", "markout_mid",
            "markout_pnl"
        };
    }

    static std::vector<std::string> entry_markout_header() {
        return {
            "ts_jst", "symbol", "side", "qty", "entry_price", "entry_mode", "entry_score", "fill_reason",
            "queue_ahead_qty", "markout_seconds", "markout_mid", "markout_pnl"
        };
    }

    static std::string horizon_key(double horizon) { return common::format_compact_decimal(horizon); }

    void flush_pending_markouts() {
        for (const auto& task : pending_markouts_) {
            const double markout_pnl = task.trade.side * (task.markout_mid - task.trade.exit_price) * task.trade.qty;
            write_row(markout_file_, {
                common::format_jst_iso(task.trade.exit_ts_ns),
                task.trade.symbol,
                std::to_string(task.trade.side),
                std::to_string(task.trade.qty),
                format_fixed(task.trade.entry_price, 4),
                format_fixed(task.trade.exit_price, 4),
                format_fixed(task.trade.realized_pnl, 2),
                task.trade.exit_reason,
                task.trade.entry_mode,
                std::to_string(task.trade.entry_score),
                task.trade.fill_reason,
                std::to_string(task.trade.queue_ahead_qty),
                common::format_compact_decimal(task.horizon),
                format_fixed(task.markout_mid, 4),
                format_fixed(markout_pnl, 2),
            });
            auto& stats = post_exit_stats_[horizon_key(task.horizon)];
            ++stats.count;
            stats.total_markout_pnl += markout_pnl;
        }
        pending_markouts_.clear();
    }

    void flush_pending_entry_markouts() {
        for (const auto& task : pending_entry_markouts_) {
            const double markout_pnl = task.side * (task.markout_mid - task.entry_price) * task.qty;
            write_row(entry_markout_file_, {
                common::format_jst_iso(task.entry_ts_ns),
                task.symbol,
                std::to_string(task.side),
                std::to_string(task.qty),
                format_fixed(task.entry_price, 4),
                task.entry_mode,
                std::to_string(task.entry_score),
                task.fill_reason,
                std::to_string(task.queue_ahead_qty),
                common::format_compact_decimal(task.horizon),
                format_fixed(task.markout_mid, 4),
                format_fixed(markout_pnl, 2),
            });
            auto& stats = entry_stats_[horizon_key(task.horizon)];
            ++stats.count;
            stats.total_markout_pnl += markout_pnl;
        }
        pending_entry_markouts_.clear();
    }

    std::filesystem::path trade_path_;
    std::filesystem::path markout_path_;
    std::filesystem::path entry_markout_path_;
    double default_markout_seconds_{30.0};
    std::vector<double> markout_horizons_;
    std::vector<double> entry_markout_horizons_;
    std::ofstream trade_file_;
    std::ofstream markout_file_;
    std::ofstream entry_markout_file_;
    int trade_count_{0};
    std::vector<PostExitMarkoutTask> pending_markouts_;
    std::vector<EntryMarkoutTask> pending_entry_markouts_;
    std::map<std::string, HorizonStats> post_exit_stats_;
    std::map<std::string, HorizonStats> entry_stats_;
};

}  // namespace kabu
