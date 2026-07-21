#include <iostream>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <zmq.h>
#include <nlohmann/json.hpp>

struct Frame {
    double vpp;
    double freq;
    double duty;
    char padding[104];
};
static_assert(sizeof(Frame) == 128, "Frame must be 128 bytes");

struct Config {
    std::string dataHost = "127.0.0.1";
    int dataPort = 9001;
    std::string notifyHost = "127.0.0.1";
    int notifyPort = 9002;
    int frameSize = sizeof(Frame);
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data-host" && i+1 < argc) cfg.dataHost = argv[++i];
        if (a == "--data-port" && i+1 < argc) cfg.dataPort = std::stoi(argv[++i]);
        if (a == "--notify-host" && i+1 < argc) cfg.notifyHost = argv[++i];
        if (a == "--notify-port" && i+1 < argc) cfg.notifyPort = std::stoi(argv[++i]);
    }
    return cfg;
}

int connectTcp(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    addr.sin_port = htons(port);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

bool recvAll(int fd, void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t r = recv(fd, (char*)buf + total, len - total, MSG_WAITALL);
        if (r <= 0) return false;
        total += r;
    }
    return true;
}

int main(int argc, char* argv[]) {
    Config cfg = parseArgs(argc, argv);

    // TCP: connect to producer
    int dataFd = connectTcp(cfg.dataHost, cfg.dataPort);
    if (dataFd < 0) return 1;
    std::cout << "[CONSUMER] TCP connected to " << cfg.dataHost << ":" << cfg.dataPort << "\n";

    // ZMQ: SUB for DATA_READY
    void* ctx = zmq_ctx_new();
    void* subSock = zmq_socket(ctx, ZMQ_SUB);
    int linger = 0;
    zmq_setsockopt(subSock, ZMQ_LINGER, &linger, sizeof(linger));
    zmq_setsockopt(subSock, ZMQ_SUBSCRIBE, "", 0);
    std::string notifyAddr = "tcp://" + cfg.notifyHost + ":" + std::to_string(cfg.notifyPort);
    if (zmq_connect(subSock, notifyAddr.c_str()) != 0) {
        std::cerr << "[CONSUMER] Failed to connect ZMQ SUB " << notifyAddr << "\n";
        return 1;
    }
    std::cout << "[CONSUMER] ZMQ SUB connected to " << notifyAddr << "\n";

    // Stats
    long totalBytes = 0;
    long batchesDone = 0;
    int errors = 0;
    auto tStart = std::chrono::steady_clock::now();

    // Event loop: poll ZMQ + TCP
    zmq_pollitem_t items[1] = {{subSock, 0, ZMQ_POLLIN, 0}};

    while (true) {
        int rc = zmq_poll(items, 1, 5000);
        if (rc < 0) break;
        if (!(items[0].revents & ZMQ_POLLIN)) {
            std::cerr << "[CONSUMER] Timeout waiting for DATA_READY\n";
            break;
        }

        // Receive ZMQ message
        zmq_msg_t zmsg;
        zmq_msg_init(&zmsg);
        zmq_msg_recv(&zmsg, subSock, 0);
        std::string raw(static_cast<const char*>(zmq_msg_data(&zmsg)), zmq_msg_size(&zmsg));
        zmq_msg_close(&zmsg);

        try {
            auto j = nlohmann::json::parse(raw);
            std::string cmd = j.value("cmd", "");

            if (cmd == "DONE") {
                long totalBatches = j.value("total_batches", 0L);
                long totalBytesExpected = j.value("total_bytes", 0L);
                std::cout << "\n[CONSUMER] Producer signaled DONE (" << totalBatches << " batches)\n";
                break;
            }

            if (cmd == "DATA_READY") {
                size_t byteCount = j["byte_count"];
                long seq = j.value("sequence", 0L);
                int frameSize = j.value("frame_size", (int)sizeof(Frame));
                int frames = j.value("frames", 0);
                size_t expectedBytes = frameSize * frames;

                if (byteCount != expectedBytes) {
                    std::cerr << "[CONSUMER] byte_count mismatch: " << byteCount
                              << " vs " << expectedBytes << "\n";
                }

                // Read batch data from TCP
                std::vector<char> buf(byteCount);
                if (!recvAll(dataFd, buf.data(), byteCount)) {
                    std::cerr << "[CONSUMER] TCP recv failed at seq " << seq << "\n";
                    break;
                }

                totalBytes += byteCount;
                batchesDone++;

                // Validate frames
                auto* frames_ptr = reinterpret_cast<Frame*>(buf.data());
                int nframes = byteCount / sizeof(Frame);
                for (int f = 0; f < nframes; f++) {
                    double expectedVpp = seq * 1000.0 + f;
                    double expectedFreq = f * 0.1;
                    double expectedDuty = 50.0;
                    if (frames_ptr[f].vpp != expectedVpp ||
                        frames_ptr[f].freq != expectedFreq ||
                        frames_ptr[f].duty != expectedDuty) {
                        if (errors < 5) {
                            std::cerr << "[CONSUMER] Data mismatch at seq=" << seq
                                      << " frame=" << f
                                      << ": vpp=" << frames_ptr[f].vpp
                                      << " (expected " << expectedVpp << ")\n";
                        }
                        errors++;
                    }
                }

                if (batchesDone % 1000 == 0) {
                    auto now = std::chrono::steady_clock::now();
                    double elapsed = std::chrono::duration<double>(now - tStart).count();
                    double mbps = totalBytes / 1024.0 / 1024.0 / elapsed;
                    std::cout << "\r[CONSUMER] " << batchesDone << " batches, "
                              << totalBytes/1024/1024 << " MB, "
                              << mbps << " MB/s   ";
                    std::cout.flush();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[CONSUMER] JSON parse error: " << e.what() << "\n";
        }
    }

    auto tEnd = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();
    double mbps = totalBytes / 1024.0 / 1024.0 / elapsedSec;
    double msgsPerSec = batchesDone / elapsedSec;

    std::cout << "\n[CONSUMER] Done.\n";
    std::cout << "[CONSUMER] Batches: " << batchesDone << "\n";
    std::cout << "[CONSUMER] Total data: " << totalBytes << " bytes (" << (totalBytes/1024/1024) << " MB)\n";
    std::cout << "[CONSUMER] Errors: " << errors << "\n";
    std::cout << "[CONSUMER] Elapsed: " << elapsedSec << " s\n";
    std::cout << "[CONSUMER] Throughput: " << mbps << " MB/s\n";
    std::cout << "[CONSUMER] DATA_READY rate: " << msgsPerSec << " msg/s\n";

    close(dataFd);
    zmq_close(subSock);
    zmq_ctx_destroy(ctx);
    return errors > 0 ? 1 : 0;
}
