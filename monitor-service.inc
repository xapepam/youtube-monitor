#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <mutex>
#include <thread>

static volatile std::sig_atomic_t interrupted = 0;

static void handle_signal(int) {
    interrupted = 1;
}

static json timestamp_utc() {
    const std::time_t now = std::time(nullptr);
    if (now == std::time_t{-1}) {
        return nullptr;
    }

    std::tm tm{};
    if (gmtime_r(&now, &tm) == nullptr) {
        return nullptr;
    }

    char buffer[32]{};
    if (std::strftime(
            buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm
        ) == 0) {
        return nullptr;
    }

    return std::string(buffer);
}

struct SharedState {
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;

    json status = {
        {"status", "starting"},
        {"speed_mbps", nullptr},
        {"low_count", 0},
        {"error_count", 0},
        {"checked_at", nullptr},
        {"detail", "Ожидание первой проверки"}
    };
};

struct WorkerSettings {
    unsigned interval;
    double low_mbps;
    long timeout_seconds;
};

static void worker(
    SharedState& shared,
    const json& config,
    const std::string& video_id,
    const WorkerSettings& settings
) {
    while (true) {
        try {
            const std::string visitor = get_visitor(video_id);
            const std::string video_url = get_video_url(config, visitor);
            const json measurement = measure_speed(video_url, settings.timeout_seconds);
            const double speed = measurement.at("speed_mbps").get<double>();

            {
                std::lock_guard<std::mutex> lock(shared.mutex);

                const int previous_low =
                    shared.status.at("low_count").get<int>();

                shared.status["status"] = "ok";
                shared.status["speed_mbps"] = speed;
                shared.status["low_count"] =
                    speed < settings.low_mbps ? previous_low + 1 : 0;
                shared.status["error_count"] = 0;
                shared.status["checked_at"] = timestamp_utc();
                shared.status["detail"] =
                    "Загружено " +
                    std::to_string(
                        measurement.at("bytes").get<std::size_t>()
                    ) +
                    " байт за " +
                    std::to_string(
                        measurement.at("seconds").get<double>()
                    ) +
                    " с" +
                    (measurement.at("timed_out").get<bool>()
                        ? " (загрузка остановлена по тайм-ауту)"
                        : "");
            }

            std::cout << "Проверка OK: " << speed << " Мбит/с"
                      << std::endl;

        } catch (const std::exception& e) {
            {
                std::lock_guard<std::mutex> lock(shared.mutex);

                const int previous_errors =
                    shared.status.at("error_count").get<int>();

                shared.status["status"] = "error";
                shared.status["speed_mbps"] = nullptr;
                shared.status["low_count"] = 0;
                shared.status["error_count"] = previous_errors + 1;
                shared.status["checked_at"] = timestamp_utc();
                shared.status["detail"] = std::string(e.what()).substr(0, 300);
            }

            std::cerr << "Ошибка проверки: " << e.what() << std::endl;
        }

        std::unique_lock<std::mutex> lock(shared.mutex);
        if (shared.wake.wait_for(
                lock,
                std::chrono::seconds(settings.interval),
                [&shared] { return shared.stopping; }
            )) {
            break;
        }
    }
}

static void send_all(int fd, const std::string& data) {
    std::size_t sent = 0;

    while (sent < data.size()) {
        const ssize_t n = send(
            fd,
            data.data() + sent,
            data.size() - sent,
            MSG_NOSIGNAL
        );

        if (n <= 0) return;
        sent += static_cast<std::size_t>(n);
    }
}

static void handle_client(int client, SharedState& shared) {
    timeval timeout{};
    timeout.tv_sec = 2;

    setsockopt(
        client, SOL_SOCKET, SO_RCVTIMEO,
        &timeout, sizeof(timeout)
    );
    setsockopt(
        client, SOL_SOCKET, SO_SNDTIMEO,
        &timeout, sizeof(timeout)
    );

    std::string request;
    char buffer[1024];

    while (request.size() < 4096 &&
           request.find('\n') == std::string::npos) {
        const ssize_t n = recv(client, buffer, sizeof(buffer), 0);

        if (n <= 0) return;
        request.append(buffer, static_cast<std::size_t>(n));
    }

    const bool valid =
        request.rfind("GET /status HTTP/", 0) == 0;

    std::string body;
    std::string status_line;

    if (valid) {
        std::lock_guard<std::mutex> lock(shared.mutex);
        body = shared.status.dump();
        status_line = "HTTP/1.1 200 OK\r\n";
    } else {
        body = R"({"error":"not found"})";
        status_line = "HTTP/1.1 404 Not Found\r\n";
    }

    const std::string response =
        status_line +
        "Content-Type: application/json; charset=utf-8\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;

    send_all(client, response);
}


static std::string parse_video_input(const std::string& input) {
    // Можно передать непосредственно ID.
    if (valid_video_id(input)) {
        return input;
    }

    const std::string https = "https://";
    const std::string http = "http://";
    std::size_t host_start = 0;

    if (input.compare(0, https.size(), https) == 0) {
        host_start = https.size();
    } else if (input.compare(0, http.size(), http) == 0) {
        host_start = http.size();
    } else {
        throw std::runtime_error(
            "YTM_VIDEO_URL: нужен ID или ссылка http(s)"
        );
    }

    const auto host_end = input.find_first_of("/?#", host_start);
    if (host_end == std::string::npos) {
        throw std::runtime_error("YTM_VIDEO_URL: отсутствует путь");
    }

    const std::string host =
        input.substr(host_start, host_end - host_start);

    std::string path_and_query = input.substr(host_end);

    // Фрагмент после # не относится к параметрам запроса.
    const auto fragment = path_and_query.find('#');
    if (fragment != std::string::npos) {
        path_and_query.resize(fragment);
    }

    const auto question = path_and_query.find('?');
    const std::string path = path_and_query.substr(0, question);

    std::string id;

    if (host == "youtu.be" && path.size() > 1 && path[0] == '/') {
        id = path.substr(1);
    } else if (
        (host == "youtube.com" ||
         host == "www.youtube.com" ||
         host == "m.youtube.com" ||
         host == "music.youtube.com") &&
        path == "/watch" &&
        question != std::string::npos
    ) {
        const std::string query = path_and_query.substr(question + 1);
        std::size_t start = 0;

        while (start <= query.size()) {
            const auto end = query.find('&', start);
            const std::string part = query.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start
            );

            if (part.compare(0, 2, "v=") == 0) {
                id = part.substr(2);
                break;
            }

            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    if (!valid_video_id(id)) {
        throw std::runtime_error(
            "YTM_VIDEO_URL: не удалось получить корректный ID видео"
        );
    }

    return id;
}

#include "visionos-client.inc"

int main() try {
    unsigned interval = 3600;

    if (const char* value = std::getenv("YTM_INTERVAL")) {
        try {
            const unsigned long parsed = std::stoul(value);

            if (parsed < 1 || parsed > 86400) {
                throw std::out_of_range("интервал");
            }

            interval = static_cast<unsigned>(parsed);
        } catch (const std::exception&) {
            std::cerr << "YTM_INTERVAL должен быть от 1 до 86400 секунд\n";
            return 1;
        }
    }


    double low_mbps = 2.5;
    long timeout_seconds = 20;

    try {
        if (const char* value = std::getenv("YTM_LOW_MBPS")) {
            std::string input(value);
            std::size_t parsed = 0;
            low_mbps = std::stod(input, &parsed);

            if (parsed != input.size() ||
                !std::isfinite(low_mbps) ||
                low_mbps <= 0 ||
                low_mbps > 10000) {
                throw std::invalid_argument("YTM_LOW_MBPS");
            }
        }

        if (const char* value = std::getenv("YTM_TIMEOUT_SECONDS")) {
            std::string input(value);
            std::size_t parsed = 0;
            timeout_seconds = std::stol(input, &parsed);

            if (parsed != input.size() ||
                timeout_seconds < 1 ||
                timeout_seconds > 3600) {
                throw std::invalid_argument("YTM_TIMEOUT_SECONDS");
            }
        }
    } catch (const std::exception&) {
        std::cerr
            << "Проверь параметры: YTM_LOW_MBPS — число от 0 до 10000 "
            << "(не включая 0), YTM_TIMEOUT_SECONDS — целое число "
            << "от 1 до 3600\n";
        return 1;
    }

    json config;
    std::string video_id;

    try {
        config = json::parse(VISIONOS_CONFIG);
        const char* value = std::getenv("YTM_VIDEO_URL");

        if (!value || !*value) {
            throw std::runtime_error(
                "Задай YTM_VIDEO_URL: ссылку на видео или его ID"
            );
        }

        video_id = parse_video_input(value);
        config.at("body")["videoId"] = video_id;

        std::cout << "Видео: " << video_id << std::endl;

        if (!valid_video_id(video_id)) {
            throw std::runtime_error("Некорректный ID видео");
        }
    } catch (const std::exception& e) {
        std::cerr << "Ошибка конфигурации: " << e.what() << "\n";
        return 1;
    }

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::cerr << "Ошибка инициализации libcurl\n";
        return 1;
    }

    const int server = socket(AF_INET, SOCK_STREAM, 0);

    if (server < 0) {
        std::perror("socket");
        curl_global_cleanup();
        return 1;
    }

    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8000);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            server,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)
        ) < 0 || listen(server, 8) < 0) {
        std::perror("bind/listen");
        close(server);
        curl_global_cleanup();
        return 1;
    }

    if (std::signal(SIGINT, handle_signal) == SIG_ERR ||
        std::signal(SIGTERM, handle_signal) == SIG_ERR) {
        std::cerr << "Не удалось установить обработчики сигналов\n";
        close(server);
        curl_global_cleanup();
        return 1;
    }

    SharedState shared;
    const WorkerSettings settings{interval, low_mbps, timeout_seconds};

    std::thread check_thread(worker, std::ref(shared),
                             std::cref(config), std::cref(video_id),
                             std::cref(settings));

    std::cout << "HTTP: http://127.0.0.1:8000/status\n"
              << "Интервал: " << interval << " с\n"
              << "Порог: " << low_mbps << " Мбит/с\n"
              << "Таймаут загрузки: " << timeout_seconds << " с"
              << std::endl;

    std::exception_ptr loop_error;

    try {
    while (!interrupted) {
        pollfd descriptor{};
        descriptor.fd = server;
        descriptor.events = POLLIN;

        const int ready = poll(&descriptor, 1, 1000);

        if (ready <= 0 ||
            (static_cast<unsigned int>(descriptor.revents) &
             static_cast<unsigned int>(POLLIN)) == 0) {
            continue;
        }

        const int client = accept(server, nullptr, nullptr);

        if (client >= 0) {
            handle_client(client, shared);
            close(client);
        }
    }
    } catch (...) {
        loop_error = std::current_exception();
    }

    close(server);

    {
        std::lock_guard<std::mutex> lock(shared.mutex);
        shared.stopping = true;
    }

    shared.wake.notify_one();
    check_thread.join();
    curl_global_cleanup();

    if (loop_error) {
        std::rethrow_exception(loop_error);
    }

    return 0;
} catch (const std::exception& e) {
    std::cerr << "Необработанная ошибка сервиса: " << e.what() << "\n";
    return 1;
} catch (...) {
    std::cerr << "Неизвестная ошибка сервиса\n";
    return 1;
}
