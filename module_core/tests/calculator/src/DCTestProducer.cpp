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
    int dataPort = 9001;
    int notifyPort = 9002;
    int frameSize = sizeof(Frame);
    int framesPerBatch = 1000;
    long totalBatches = 10000;
};

Config parseArgs(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--data-port" && i+1 < argc) cfg.dataPort = std::stoi(argv[++i]);
        if (a == "--notify-port" && i+1 < argc) cfg.notifyPort = std::stoi(argv[++i]);
        if (a == "--frames-per-batch" && i+1 < argc) cfg.framesPerBatch = std::stoi(argv[++i]);
        if (a == "--total-batches" && i+1 < argc) cfg.totalBatches = std::stol(argv[++i]);
    }
    return cfg;
}

int createTcpServer(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 1) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

int main(int argc, char* argv[]) {
    Config cfg = parseArgs(argc, argv);
    size_t batchBytes = cfg.frameSize * cfg.framesPerBatch;

    // TCP: bind + listen + accept
    int listenFd = createTcpServer(cfg.dataPort);
    if (listenFd < 0) return 1;
    std::cout << "[PRODUCER] TCP listening on port " << cfg.dataPort << "\n";

    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);
    int dataFd = accept(listenFd, (struct sockaddr*)&clientAddr, &clientLen);
    if (dataFd < 0) { perror("accept"); close(listenFd); return 1; }
    close(listenFd);
    std::cout << "[PRODUCER] TCP client connected\n";

    // ZMQ: PUB for DATA_READY
    void* ctx = zmq_ctx_new();
    void* pubSock = zmq_socket(ctx, ZMQ_PUB);
    int linger = 0;
    zmq_setsockopt(pubSock, ZMQ_LINGER, &linger, sizeof(linger));
    std::string notifyAddr = "tcp://*:" + std::to_string(cfg.notifyPort);
    if (zmq_bind(pubSock, notifyAddr.c_str()) != 0) {
        std::cerr << "[PRODUCER] Failed to bind ZMQ PUB " << notifyAddr << "\n";
        return 1;
    }
    std::cout << "[PRODUCER] ZMQ PUB on " << notifyAddr << "\n";

    // Wait for SUB to connect (slow joiner)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Prepare batch data
    std::vector<char> buf(batchBytes);
    auto* frames = reinterpret_cast<Frame*>(buf.data());

    std::cout << "[PRODUCER] Sending " << cfg.totalBatches << " batches x " << cfg.framesPerBatch
              << " frames (" << batchBytes << " bytes/batch) = "
              << (cfg.totalBatches * batchBytes / 1024.0 / 1024.0 / 1024.0) << " GB total\n";

    auto tStart = std::chrono::steady_clock::now();
    long bytesSent = 0;

    for (long b = 0; b < cfg.totalBatches; b++) {
        for (int f = 0; f < cfg.framesPerBatch; f++) {
            frames[f].vpp = b * 1000.0 + f;
            frames[f].freq = f * 0.1;
            frames[f].duty = 50.0;
        }

        // TCP: send batch data
        ssize_t sent = send(dataFd, buf.data(), batchBytes, 0);
        if (sent < 0) { perror("send"); break; }
        bytesSent += sent;

        // ZMQ: publish DATA_READY notification
        nlohmann::json notify;
        notify["cmd"] = "DATA_READY";
        notify["byte_count"] = batchBytes;
        notify["sequence"] = b;
        notify["frame_size"] = cfg.frameSize;
        notify["frames"] = cfg.framesPerBatch;
        std::string payload = notify.dump();
        zmq_send(pubSock, payload.data(), payload.size(), 0);
    }

    auto tEnd = std::chrono::steady_clock::now();
    double elapsedSec = std::chrono::duration<double>(tEnd - tStart).count();

    // Send done notification
    nlohmann::json done;
    done["cmd"] = "DONE";
    done["total_batches"] = cfg.totalBatches;
    done["total_bytes"] = bytesSent;
    zmq_send(pubSock, done.dump().data(), done.dump().size(), 0);

    double mbps = bytesSent / 1024.0 / 1024.0 / elapsedSec;
    double msgsPerSec = cfg.totalBatches / elapsedSec;
    std::cout << "\n[PRODUCER] Done. " << bytesSent << " bytes in " << elapsedSec << "s\n";
    std::cout << "[PRODUCER] Throughput: " << mbps << " MB/s\n";
    std::cout << "[PRODUCER] Messages: " << msgsPerSec << " DATA_READY/s\n";

    close(dataFd);
    zmq_close(pubSock);
    zmq_ctx_destroy(ctx);
    return 0;
}
