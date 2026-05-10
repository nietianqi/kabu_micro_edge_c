#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/common/time.hpp"
#include "kabu_micro_edge/strategy.hpp"

namespace kabu::telemetry {

class JsonlDiagnosticsWriter {
  public:
    explicit JsonlDiagnosticsWriter(std::filesystem::path path) : path_(std::move(path)) {}

    void open() {
        if (path_.empty()) {
            return;
        }
        if (path_.has_parent_path()) {
            std::filesystem::create_directories(path_.parent_path());
        }
        out_.open(path_, std::ios::app);
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (out_.is_open()) {
            out_.flush();
            out_.close();
        }
    }

    [[nodiscard]] bool enabled() const { return !path_.empty(); }
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

    void write_decision(const std::string& symbol, const strategy::DecisionTrace& trace) {
        write_event({
            {"type", "entry_decision"},
            {"symbol", symbol},
            {"ts_ns", trace.ts_ns},
            {"ts_jst", trace.ts_ns > 0 ? common::format_jst_iso(trace.ts_ns) : std::string()},
            {"decision", trace.to_json()},
        });
    }

    void write_heartbeat(const nlohmann::json& status_snapshot, std::int64_t ts_ns) {
        write_event({
            {"type", "heartbeat"},
            {"ts_ns", ts_ns},
            {"ts_jst", ts_ns > 0 ? common::format_jst_iso(ts_ns) : std::string()},
            {"status", status_snapshot},
        });
    }

  private:
    void write_event(const nlohmann::json& event) {
        if (path_.empty()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!out_.is_open()) {
            open();
        }
        if (!out_.is_open()) {
            return;
        }
        out_ << event.dump() << "\n";
        out_.flush();
    }

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::ofstream out_;
};

}  // namespace kabu::telemetry
