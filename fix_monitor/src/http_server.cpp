#include "fixmon/http_server.hpp"

#include <cstring>
#include <string>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSESOCK ::close
#endif

namespace fixmon {

namespace {

void send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = ::send(fd, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

std::string http_response(const std::string& body, const char* content_type) {
    std::string head = "HTTP/1.1 200 OK\r\nContent-Type: ";
    head += content_type;
    head += "\r\nContent-Length: " + std::to_string(body.size());
    head += "\r\nConnection: close\r\n\r\n";
    return head + body;
}

}  // namespace

HttpServer::HttpServer(uint16_t port, Handler metrics, Handler sessions)
    : port_(port), metrics_(std::move(metrics)), sessions_(std::move(sessions)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start(std::string& err) {
#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { err = "WSAStartup failed"; return false; }
#endif
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) { err = "socket() failed"; return false; }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port_);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        err = "bind() failed on port " + std::to_string(port_);
        CLOSESOCK(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) < 0) {
        err = "listen() failed";
        CLOSESOCK(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    running_.store(true);
    thread_ = std::thread(&HttpServer::run, this);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (listen_fd_ >= 0) {
        CLOSESOCK(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void HttpServer::run() {
    while (running_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        socklen_t   len = sizeof(peer);
        int fd = static_cast<int>(::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len));
        if (fd < 0) {
            if (!running_.load(std::memory_order_relaxed)) break;
            continue;
        }
        handle_client(fd);
        CLOSESOCK(fd);
    }
}

void HttpServer::handle_client(int fd) {
    char buf[2048];
    auto n = ::recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    std::string req(buf);
    std::string path = "/";
    size_t sp1 = req.find(' ');
    if (sp1 != std::string::npos) {
        size_t sp2 = req.find(' ', sp1 + 1);
        if (sp2 != std::string::npos) path = req.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    if (path == "/metrics") {
        send_all(fd, http_response(metrics_(), "text/plain; version=0.0.4; charset=utf-8"));
    } else if (path == "/sessions") {
        send_all(fd, http_response(sessions_(), "application/json"));
    } else if (path == "/healthz") {
        send_all(fd, http_response("ok\n", "text/plain"));
    } else {
        std::string body = "fixmon\n/metrics\n/sessions\n/healthz\n";
        send_all(fd, http_response(body, "text/plain"));
    }
}

}  // namespace fixmon
