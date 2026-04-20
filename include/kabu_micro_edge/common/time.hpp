#pragma once

#include <cstdint>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>

namespace kabu::common {

constexpr std::int64_t days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

constexpr std::tuple<int, unsigned, unsigned> civil_from_days(std::int64_t z) noexcept {
    z += 719468;
    const auto era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const auto yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const auto year = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const auto doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const auto mp = (5 * doy + 2) / 153;
    const auto day = doy - (153 * mp + 2) / 5 + 1;
    const auto month = mp < 10 ? mp + 3 : mp - 9;
    return {year + (month <= 2), month, day};
}

inline std::int64_t parse_iso8601_to_ns(const std::string& value) {
    static const std::regex pattern(
        R"(^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?(?:Z|([+-])(\d{2}):(\d{2}))$)"
    );
    std::smatch match;
    if (!std::regex_match(value, match, pattern)) {
        return 0;
    }

    const int year = std::stoi(match[1].str());
    const unsigned month = static_cast<unsigned>(std::stoul(match[2].str()));
    const unsigned day = static_cast<unsigned>(std::stoul(match[3].str()));
    const int hour = std::stoi(match[4].str());
    const int minute = std::stoi(match[5].str());
    const int second = std::stoi(match[6].str());

    std::int64_t fractional_ns = 0;
    if (match[7].matched) {
        std::string fraction = match[7].str();
        while (fraction.size() < 9) {
            fraction.push_back('0');
        }
        if (fraction.size() > 9) {
            fraction.resize(9);
        }
        fractional_ns = std::stoll(fraction);
    }

    int offset_seconds = 0;
    if (match[8].matched) {
        const int sign = match[8].str() == "-" ? -1 : 1;
        offset_seconds = sign * (std::stoi(match[9].str()) * 3600 + std::stoi(match[10].str()) * 60);
    }

    const auto days = days_from_civil(year, month, day);
    const std::int64_t seconds = days * 86400 + hour * 3600 + minute * 60 + second - offset_seconds;
    return seconds * 1'000'000'000LL + fractional_ns;
}

inline std::string format_jst_iso(std::int64_t ts_ns) {
    constexpr std::int64_t jst_offset_ns = 9LL * 3600LL * 1'000'000'000LL;
    const std::int64_t adjusted_ns = ts_ns + jst_offset_ns;
    const std::int64_t total_seconds = adjusted_ns / 1'000'000'000LL;
    const std::int64_t days = total_seconds / 86400LL;
    const int seconds_of_day = static_cast<int>(total_seconds % 86400LL);
    const auto [year, month, day] = civil_from_days(days);

    std::ostringstream out;
    out << std::setfill('0')
        << std::setw(4) << year
        << "-" << std::setw(2) << month
        << "-" << std::setw(2) << day
        << "T" << std::setw(2) << seconds_of_day / 3600
        << ":" << std::setw(2) << (seconds_of_day % 3600) / 60
        << ":" << std::setw(2) << seconds_of_day % 60
        << "+09:00";
    return out.str();
}

inline std::tuple<int, int, int> jst_day_key(std::int64_t ts_ns) {
    constexpr std::int64_t jst_offset_ns = 9LL * 3600LL * 1'000'000'000LL;
    const std::int64_t adjusted_ns = ts_ns + jst_offset_ns;
    const std::int64_t total_seconds = adjusted_ns / 1'000'000'000LL;
    const std::int64_t days = total_seconds / 86400LL;
    const auto [year, month, day] = civil_from_days(days);
    return {year, static_cast<int>(month), static_cast<int>(day)};
}

inline std::string format_compact_decimal(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    std::string text = out.str();
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    return text.empty() ? "0" : text;
}

}  // namespace kabu::common
