#ifndef MSM_DATACHANNEL_H
#define MSM_DATACHANNEL_H

#include <string>
#include <functional>

namespace msm {

class DataChannel {
public:
    enum Role { PRODUCER, CONSUMER };

    struct Config {
        std::string host = "127.0.0.1";
        int port = 9100;
        int timeoutMs = 5000;
    };

    DataChannel(Role role, const Config& cfg);
    ~DataChannel();
    DataChannel(const DataChannel&) = delete;
    DataChannel& operator=(const DataChannel&) = delete;

    bool start();
    bool send(const void* data, size_t len);
    bool recvAll(void* buf, size_t len);
    void close();
    bool isConnected() const { return fd_ >= 0; }
    bool isHealthy();  // TCP_INFO probe
    bool reconnect();  // close and re-establish

    size_t available();  // bytes available to read

private:
    Role role_;
    Config cfg_;
    int fd_ = -1;
    int listenFd_ = -1;
};

} // namespace msm

#endif
