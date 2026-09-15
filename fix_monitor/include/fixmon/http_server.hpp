#pragma once
//
// Small blocking HTTP server for the Prometheus scrape endpoint.
//
// A scrape is one request every 15s or so; a thread-per-connection loop is more
// than adequate and avoids pulling in a web framework. Endpoints:
//   GET /metrics  - Prometheus text exposition
//   GET /sessions - session state as JSON, handy for debugging
//   GET /healthz
//
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace fixmon {

class HttpServer {
public:
    using Handler = std::function<std::string()>;

    HttpServer(uint16_t port, Handler metrics, Handler sessions);
    ~HttpServer();

    bool start(std::string& err);
    void stop();

private:
    void run();
    void handle_client(int fd);

    uint16_t          port_;
    Handler           metrics_;
    Handler           sessions_;
    int               listen_fd_ = -1;
    std::thread       thread_;
    std::atomic<bool> running_{false};
};

}  // namespace fixmon
