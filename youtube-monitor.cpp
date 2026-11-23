#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using json = nlohmann::json;

constexpr std::size_t TEXT_LIMIT = std::size_t{8} * 1024 * 1024;
constexpr std::size_t VIDEO_LIMIT = 5'000'000;

const char* WEB_USER_AGENT =
    "Mozilla/5.0 (X11; Linux x86_64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/131.0.0.0 Safari/537.36";

struct CurlDeleter {
    void operator()(CURL* curl) const {
        if (curl) curl_easy_cleanup(curl);
    }
};

struct HeadersDeleter {
    void operator()(curl_slist* headers) const {
        if (headers) curl_slist_free_all(headers);
    }
};

using CurlPtr = std::unique_ptr<CURL, CurlDeleter>;
using HeadersPtr = std::unique_ptr<curl_slist, HeadersDeleter>;

struct TextBuffer {
    std::string data;
    bool too_large = false;
};

struct VideoCounter {
    std::size_t bytes = 0;
    bool limit_reached = false;
};

std::size_t receive_text(
    char* data, std::size_t size, std::size_t count, void* userdata
) {
    auto& buffer = *static_cast<TextBuffer*>(userdata);
    const std::size_t bytes = size * count;

    if (bytes > TEXT_LIMIT - buffer.data.size()) {
        buffer.too_large = true;
        return 0;
    }

    buffer.data.append(data, bytes);
    return bytes;
}

std::size_t receive_video(
    char*, std::size_t size, std::size_t count, void* userdata
) {
    auto& counter = *static_cast<VideoCounter*>(userdata);
    const std::size_t bytes = size * count;
    const std::size_t remaining = VIDEO_LIMIT - counter.bytes;

    counter.bytes += std::min(bytes, remaining);

    if (counter.bytes >= VIDEO_LIMIT) {
        counter.limit_reached = true;
        return 0;
    }

    return bytes;
}

CurlPtr make_curl(char* error) {
    CurlPtr curl(curl_easy_init());

    if (!curl) {
        throw std::runtime_error("Не удалось создать CURL");
    }

    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    return curl;
}

void add_header(HeadersPtr& headers, const std::string& value) {
    curl_slist* original = headers.release();
    curl_slist* updated = curl_slist_append(original, value.c_str());

    // При ошибке curl_slist_append исходный список остаётся действительным.
    headers.reset(updated ? updated : original);

    if (!updated) {
        throw std::runtime_error("Не удалось добавить HTTP-заголовок");
    }
}

std::string curl_error(CURLcode result, const char* error) {
    return error[0] ? error : curl_easy_strerror(result);
}

bool valid_video_id(const std::string& id) {
    return id.size() == 11 &&
        std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == '_' || c == '-';
        });
}

std::string extract_visitor(const std::string& html) {
    for (const char* field : {"\"VISITOR_DATA\"", "\"visitorData\""}) {
        const std::string marker(field);
        std::size_t position = 0;

        while ((position = html.find(marker, position)) != std::string::npos) {
            const auto colon = html.find(':', position + marker.size());

            if (colon == std::string::npos || colon - position > 100) {
                position += marker.size();
                continue;
            }

            std::size_t start = colon + 1;

            while (start < html.size() &&
                   (html[start] == ' ' || html[start] == '\n' ||
                    html[start] == '\r' || html[start] == '\t')) {
                ++start;
            }

            if (start >= html.size() || html[start] != '"') {
                position += marker.size();
                continue;
            }

            bool escaped = false;

            for (std::size_t i = start + 1; i < html.size(); ++i) {
                const char c = html[i];

                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    // Некорректный кандидат пропускаем и ищем следующий.
                    const auto candidate = json::parse(
                        html.substr(start, i - start + 1),
                        nullptr,
                        false
                    );

                    if (candidate.is_string()) {
                        const auto value = candidate.get<std::string>();

                        if (!value.empty() &&
                            value.find_first_of("\r\n") == std::string::npos) {
                            return value;
                        }
                    }

                    break;
                }
            }

            position += marker.size();
        }
    }

    throw std::runtime_error("Visitor ID на странице не найден");
}

std::string get_visitor(const std::string& video_id) {
    const std::string url =
        "https://www.youtube.com/watch?v=" + video_id;

    char error[CURL_ERROR_SIZE] = {};
    auto curl = make_curl(error);
    TextBuffer page;
    HeadersPtr headers;

    add_header(headers, "Accept-Language: en-US,en;q=0.9");

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, WEB_USER_AGENT);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_text);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &page);

    const CURLcode result = curl_easy_perform(curl.get());

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);

    if (page.too_large) {
        throw std::runtime_error("Страница YouTube превысила 8 МиБ");
    }

    if (result != CURLE_OK) {
        throw std::runtime_error(
            "Ошибка загрузки страницы: " + curl_error(result, error)
        );
    }

    if (status != 200) {
        throw std::runtime_error(
            "Страница YouTube вернула HTTP " + std::to_string(status)
        );
    }

    return extract_visitor(page.data);
}

std::string get_video_url(
    const json& config, const std::string& visitor
) {
    const std::string endpoint = config.at("url").get<std::string>();
    const std::string body = config.at("body").dump();

    if (endpoint.rfind("https://www.youtube.com/", 0) != 0) {
        throw std::runtime_error("Неожиданный адрес API в конфигурации");
    }

    char error[CURL_ERROR_SIZE] = {};
    auto curl = make_curl(error);
    TextBuffer response;
    HeadersPtr headers;

    for (auto it = config.at("headers").begin();
         it != config.at("headers").end(); ++it) {
        if (!it.value().is_string()) continue;

        std::string lower = it.key();
        std::transform(lower.begin(), lower.end(), lower.begin(),
            [](unsigned char c) {
                return c >= 'A' && c <= 'Z'
                    ? static_cast<char>(c - 'A' + 'a')
                    : static_cast<char>(c);
            });

        if (lower == "x-goog-visitor-id" ||
            lower == "content-length" ||
            lower == "transfer-encoding" ||
            lower == "host") {
            continue;
        }

        const std::string value = it.value().get<std::string>();

        if (it.key().find_first_of("\r\n") != std::string::npos ||
            value.find_first_of("\r\n") != std::string::npos) {
            throw std::runtime_error("Некорректный HTTP-заголовок");
        }

        add_header(headers, it.key() + ": " + value);
    }

    add_header(headers, "X-Goog-Visitor-Id: " + visitor);

    curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(
        curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(body.size())
    );
    curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_text);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);

    const CURLcode result = curl_easy_perform(curl.get());

    long http_status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);

    if (response.too_large) {
        throw std::runtime_error("Ответ API превысил 8 МиБ");
    }

    if (result != CURLE_OK) {
        throw std::runtime_error(
            "Ошибка запроса к API: " + curl_error(result, error)
        );
    }

    if (http_status != 200) {
        throw std::runtime_error(
            "API вернул HTTP " + std::to_string(http_status)
        );
    }

    const json player = json::parse(response.data);
    const auto status = player.value("playabilityStatus", json::object());

    if (status.value("status", "") != "OK") {
        throw std::runtime_error(
            "Видео: " + status.value("status", "UNKNOWN") +
            ", " + status.value("reason", "причина не указана")
        );
    }

    const auto streaming = player.value("streamingData", json::object());

    std::string selected_url;
    int selected_height = 0;
    std::int64_t selected_bitrate = -1;

    for (const char* section : {"adaptiveFormats", "formats"}) {
        if (!streaming.contains(section) ||
            !streaming[section].is_array()) {
            continue;
        }

        for (const auto& format : streaming[section]) {
            if (!format.is_object()) continue;

            const std::string mime = format.value("mimeType", "");
            const int height = format.value("height", 0);

            if (mime.rfind("video/", 0) != 0 ||
                height <= 0 || height > 1080 ||
                !format.contains("url") ||
                !format["url"].is_string()) {
                continue;
            }

            const std::string url = format["url"].get<std::string>();
            const std::int64_t bitrate =
                format.value("bitrate", std::int64_t{0});

            if (url.rfind("https://", 0) != 0) continue;

            if (height > selected_height ||
                (height == selected_height && bitrate > selected_bitrate)) {
                selected_url = url;
                selected_height = height;
                selected_bitrate = bitrate;
            }
        }
    }

    if (selected_url.empty()) {
        throw std::runtime_error("Прямая ссылка на видео не найдена");
    }

    std::cout << "Выбран формат: " << selected_height << "p\n";
    return selected_url;
}


struct MeasurementResult {
    CURLcode result = CURLE_FAILED_INIT;
    bool limit_reached = false;
    long http_status = 0;
    std::size_t bytes = 0;
    double seconds = 0.0;
    std::string error_message{};
};

json classify_measurement(const MeasurementResult& measurement) {
    const auto& [
        result,
        limit_reached,
        http_status,
        bytes,
        seconds,
        error_message
    ] = measurement;
    const bool stopped_at_limit =
        result == CURLE_WRITE_ERROR && limit_reached;

    const bool timed_out = result == CURLE_OPERATION_TIMEDOUT;

    if (http_status != 200 && http_status != 206) {
        if (http_status == 0 && result != CURLE_OK) {
            throw std::runtime_error(
                "Ошибка загрузки видео: " + error_message
            );
        }

        throw std::runtime_error(
            "Сервер видео вернул HTTP " + std::to_string(http_status)
        );
    }

    if (result != CURLE_OK && !stopped_at_limit && !timed_out) {
        throw std::runtime_error(
            "Ошибка загрузки видео: " + error_message
        );
    }

    if (bytes == 0 || seconds <= 0) {
        throw std::runtime_error(
            timed_out
                ? "Таймаут загрузки: данные видео не получены"
                : "Данные видео не получены"
        );
    }

    const double mbps =
        static_cast<double>(bytes) * 8.0 / seconds / 1'000'000.0;

    return {
        {"speed_mbps", mbps},
        {"bytes", bytes},
        {"seconds", seconds},
        {"timed_out", timed_out}
    };
}

json measure_speed(const std::string& url, long timeout_seconds) {
    char error[CURL_ERROR_SIZE] = {};
    auto curl = make_curl(error);
    VideoCounter counter;

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, WEB_USER_AGENT);
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_MAXREDIRS, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl.get(), CURLOPT_RANGE, "0-4999999");
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_video);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &counter);

    const CURLcode result = curl_easy_perform(curl.get());

    long http_status = 0;
    double seconds = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
    curl_easy_getinfo(curl.get(), CURLINFO_TOTAL_TIME, &seconds);

    return classify_measurement(MeasurementResult{
        result,
        counter.limit_reached,
        http_status,
        counter.bytes,
        seconds,
        curl_error(result, error)
    });
}

#include "monitor-service.inc"
