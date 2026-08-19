// config_handoff.cpp
#include "config_handoff.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

namespace {
bool readFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(ifs),
               std::istreambuf_iterator<char>());
    return true;
}

bool atomicWrite(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream ofs(tmp, std::ios::binary);
        if (!ofs.is_open()) return false;
        ofs << content;
        ofs.flush();
        int fd = ::open(tmp.c_str(), O_RDONLY);
        if (fd >= 0) { ::fsync(fd); ::close(fd); }
        else { ofs.close(); ::unlink(tmp.c_str()); return false; }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}
} // namespace

int ensureRuntimeConfig(const std::string& initPath,
                        const std::string& pendingPath,
                        const std::string& runtimePath) {
    std::string pending;
    bool hasPending = readFile(pendingPath, pending) && !pending.empty();

    if (hasPending) {
        if (!atomicWrite(runtimePath, pending)) {
            std::cerr << "[config] FAILED to copy pending -> runtime: "
                      << runtimePath << "\n";
            return 1;
        }
        std::cout << "[config] pending -> runtime: " << runtimePath << "\n";
        return 0;
    }

    std::string runtime;
    if (readFile(runtimePath, runtime) && !runtime.empty()) {
        std::cout << "[config] no pending, using existing runtime: "
                  << runtimePath << "\n";
        return 0;
    }

    std::string init;
    if (!readFile(initPath, init) || init.empty()) {
        std::cerr << "[config] no runtime and no init seed: " << initPath << "\n";
        return 1;
    }
    if (!atomicWrite(runtimePath, init)) {
        std::cerr << "[config] FAILED to seed runtime from init: "
                  << runtimePath << "\n";
        return 1;
    }
    std::cout << "[config] seeded runtime from init (no pending yet)\n";
    return 0;
}
