#pragma once

#include <cctype>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

#include <nlohmann/json.hpp>

#include "kabu_micro_edge/gateway_client.hpp"

namespace kabu::gateway {

struct ParsedHttpUrl {
    bool secure{false};
    std::string host;
    std::uint16_t port{0};
    std::string target{"/"};
};

inline bool is_unreserved_url_char(unsigned char value) {
    return std::isalnum(value) != 0 || value == '-' || value == '_' || value == '.' || value == '~';
}

inline std::string percent_encode_component(std::string_view value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (unsigned char ch : value) {
        if (is_unreserved_url_char(ch)) {
            encoded << static_cast<char>(ch);
            continue;
        }
        encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
    }
    return encoded.str();
}

inline std::string query_value_to_string(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_number_integer() || value.is_number_unsigned()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_float()) {
        return value.dump();
    }
    if (value.is_null()) {
        return "";
    }
    throw std::runtime_error("HTTP query parameters must be scalar values or arrays of scalar values");
}

inline std::string build_query_string(const nlohmann::json& params) {
    if (!params.is_object() || params.empty()) {
        return "";
    }

    std::vector<std::string> parts;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const std::string encoded_key = percent_encode_component(it.key());
        if (it.value().is_null()) {
            continue;
        }
        if (it.value().is_array()) {
            for (const auto& item : it.value()) {
                parts.push_back(encoded_key + "=" + percent_encode_component(query_value_to_string(item)));
            }
            continue;
        }
        parts.push_back(encoded_key + "=" + percent_encode_component(query_value_to_string(it.value())));
    }

    std::ostringstream joined;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            joined << '&';
        }
        joined << parts[index];
    }
    return joined.str();
}

inline ParsedHttpUrl parse_http_url(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::runtime_error("invalid HTTP URL: missing scheme in " + url);
    }

    ParsedHttpUrl parsed;
    const std::string scheme = url.substr(0, scheme_end);
    if (scheme == "http") {
        parsed.secure = false;
        parsed.port = 80;
    } else if (scheme == "https") {
        parsed.secure = true;
        parsed.port = 443;
    } else {
        throw std::runtime_error("unsupported URL scheme for live REST executor: " + scheme);
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
        throw std::runtime_error("invalid HTTP URL: missing host in " + url);
    }
    if (parsed.target.empty()) {
        parsed.target = "/";
    }
    return parsed;
}

#ifdef _WIN32

class WinHttpHandle {
  public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.release()) {}
    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    ~WinHttpHandle() { reset(); }

    [[nodiscard]] HINTERNET get() const { return handle_; }
    [[nodiscard]] explicit operator bool() const { return handle_ != nullptr; }

    [[nodiscard]] HINTERNET release() {
        HINTERNET raw = handle_;
        handle_ = nullptr;
        return raw;
    }

    void reset(HINTERNET next = nullptr) {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
        handle_ = next;
    }

  private:
    HINTERNET handle_{nullptr};
};

inline std::wstring utf8_to_utf16(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-8 text to UTF-16");
    }
    std::wstring output(size, L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size) <= 0) {
        throw std::runtime_error("failed to convert UTF-8 text to UTF-16");
    }
    return output;
}

inline std::string utf16_to_utf8(const std::wstring& input) {
    if (input.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert UTF-16 text to UTF-8");
    }
    std::string output(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()), output.data(), size, nullptr, nullptr) <= 0) {
        throw std::runtime_error("failed to convert UTF-16 text to UTF-8");
    }
    return output;
}

inline std::string winhttp_last_error_message(const std::string& context) {
    const DWORD error_code = GetLastError();
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
        detail = utf16_to_utf8(std::wstring(buffer, length));
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
            detail.pop_back();
        }
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    std::ostringstream message;
    message << context << " (WinHTTP error " << error_code;
    if (!detail.empty()) {
        message << ": " << detail;
    }
    message << ")";
    return message.str();
}

inline std::string read_winhttp_response_body(HINTERNET request) {
    std::string body;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw std::runtime_error(winhttp_last_error_message("failed to query response body size"));
        }
        if (available == 0) {
            break;
        }
        std::string chunk(static_cast<std::size_t>(available), '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            throw std::runtime_error(winhttp_last_error_message("failed to read response body"));
        }
        chunk.resize(static_cast<std::size_t>(read));
        body += chunk;
    }
    return body;
}

inline TransportResponse perform_live_rest_request(
    const std::string& method,
    const std::string& url,
    const nlohmann::json& json_body,
    const nlohmann::json& params,
    bool include_token,
    const std::string& api_token
) {
    if (include_token && api_token.empty()) {
        throw std::runtime_error("authenticated kabusapi request requires an API token");
    }

    const ParsedHttpUrl parsed = parse_http_url(url);
    const std::string query = build_query_string(params);
    const std::string target = query.empty()
                                   ? parsed.target
                                   : parsed.target + (parsed.target.find('?') == std::string::npos ? "?" : "&") + query;
    const std::string body = (method == "GET" || method == "DELETE") ? std::string() : json_body.dump();

    WinHttpHandle session(
        WinHttpOpen(L"kabu_micro_edge/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
    );
    if (!session) {
        throw std::runtime_error(winhttp_last_error_message("failed to create WinHTTP session"));
    }

    WinHttpHandle connection(WinHttpConnect(session.get(), utf8_to_utf16(parsed.host).c_str(), parsed.port, 0));
    if (!connection) {
        throw std::runtime_error(winhttp_last_error_message("failed to connect to kabusapi host"));
    }

    const DWORD open_flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle request(
        WinHttpOpenRequest(
            connection.get(),
            utf8_to_utf16(method).c_str(),
            utf8_to_utf16(target).c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            open_flags
        )
    );
    if (!request) {
        throw std::runtime_error(winhttp_last_error_message("failed to create HTTP request"));
    }

    if (!WinHttpSetTimeouts(request.get(), 5000, 5000, 10000, 10000)) {
        throw std::runtime_error(winhttp_last_error_message("failed to configure HTTP timeouts"));
    }

    std::wstring headers = L"Accept: application/json\r\n";
    if (method != "GET" && method != "DELETE") {
        headers += L"Content-Type: application/json\r\n";
    }
    if (include_token) {
        headers += L"X-API-KEY: " + utf8_to_utf16(api_token) + L"\r\n";
    }

    const DWORD body_size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(
            request.get(),
            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
            headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
            body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
            body_size,
            body_size,
            0
        )) {
        throw std::runtime_error(winhttp_last_error_message("failed to send kabusapi request"));
    }

    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error(winhttp_last_error_message("failed to receive kabusapi response"));
    }

    DWORD status_code = 0;
    DWORD status_code_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(
            request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &status_code_size,
            WINHTTP_NO_HEADER_INDEX
        )) {
        throw std::runtime_error(winhttp_last_error_message("failed to read kabusapi status code"));
    }

    const std::string response_body = read_winhttp_response_body(request.get());
    nlohmann::json payload = nlohmann::json::object();
    if (!response_body.empty()) {
        try {
            payload = nlohmann::json::parse(response_body);
        } catch (...) {
            payload = nlohmann::json{{"raw_body", response_body}};
        }
    }

    return TransportResponse{static_cast<int>(status_code), payload};
}

inline KabuRestClient::RequestExecutor make_live_rest_request_executor(KabuRestClient& client) {
    return [&client](
               const std::string& method,
               const std::string& url,
               const nlohmann::json& json_body,
               const nlohmann::json& params,
               bool include_token,
               RequestLane
           ) {
        return perform_live_rest_request(method, url, json_body, params, include_token, client.token());
    };
}

#else

inline KabuRestClient::RequestExecutor make_live_rest_request_executor(KabuRestClient&) {
    return [](
               const std::string&,
               const std::string&,
               const nlohmann::json&,
               const nlohmann::json&,
               bool,
               RequestLane
           ) -> TransportResponse {
        throw std::runtime_error("live REST executor is only implemented for Windows");
    };
}

#endif

}  // namespace kabu::gateway
