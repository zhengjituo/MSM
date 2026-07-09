#include "DataChannel.h"
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <netinet/tcp.h>
#include <chrono>
#include <thread>
#include <iostream>

namespace msm {

DataChannel::DataChannel(Role role, const Config& cfg)
    : role_(role), cfg_(cfg) {}

DataChannel::~DataChannel() { close(); }

static void setSocketOpts(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    struct timeval tv;
    tv.tv_sec = 5; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
}

bool DataChannel::start() {
    if (role_ == PRODUCER) {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) { perror("socket"); return false; }
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(cfg_.port);
        if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind"); ::close(listenFd_); listenFd_ = -1; return false;
        }
        if (listen(listenFd_, 1) < 0) {
            perror("listen"); ::close(listenFd_); listenFd_ = -1; return false;
        }
        struct pollfd pfd = {listenFd_, POLLIN, 0};
        int rc = poll(&pfd, 1, cfg_.timeoutMs);
        if (rc <= 0) {
            std::cerr << "[DataChannel] Accept timeout\n";
            ::close(listenFd_); listenFd_ = -1; return false;
        }
        sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        fd_ = accept(listenFd_, (sockaddr*)&clientAddr, &clientLen);
        ::close(listenFd_); listenFd_ = -1;
        if (fd_ < 0) { perror("accept"); return false; }
        setSocketOpts(fd_);
        return true;
    } else {
        auto tryConnect = [&]() -> bool {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return false;
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);
            addr.sin_port = htons(cfg_.port);
            if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
                ::close(fd); return false;
            }
            setSocketOpts(fd);
            fd_ = fd;
            return true;
        };
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(cfg_.timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (tryConnect()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }
}

bool DataChannel::send(const void* data, size_t len) {
    if (fd_ < 0) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(fd_, (const char*)data + total, len - total, 0);
        if (n <= 0) { perror("send"); return false; }
        total += n;
    }
    return true;
}

bool DataChannel::recvAll(void* buf, size_t len) {
    if (fd_ < 0) return false;
    size_t total = 0;
    while (total < len) {
        ssize_t n = ::recv(fd_, (char*)buf + total, len - total, MSG_WAITALL);
        if (n <= 0) return false;
        total += n;
    }
    return true;
}

size_t DataChannel::available() {
    if (fd_ < 0) return 0;
    struct pollfd pfd = {fd_, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return 0;
    size_t count = 0;
    ioctl(fd_, FIONREAD, &count);
    return count;
}

bool DataChannel::isHealthy() {
    if (fd_ < 0) return false;
    struct tcp_info info;
    socklen_t len = sizeof(info);
    if (getsockopt(fd_, IPPROTO_TCP, TCP_INFO, &info, &len) < 0)
        return false;
    return info.tcpi_state == TCP_ESTABLISHED;
}

bool DataChannel::reconnect() {
    ::close(fd_); fd_ = -1;
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
    return start();
}

void DataChannel::close() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    if (listenFd_ >= 0) { ::close(listenFd_); listenFd_ = -1; }
}

} // namespace msm
