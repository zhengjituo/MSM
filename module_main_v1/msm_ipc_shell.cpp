#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <poll.h>
#include <csignal>

static int connect_socket(const char* path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    const char* sock_path = argc > 1 ? argv[1] : "/tmp/msm_daq.sock";
    int fd = connect_socket(sock_path);
    if (fd < 0) return 1;

    std::cout << "Connected to " << sock_path << "\n"
              << "Commands: start  stop  status  reload  quit  <any MSG string>\n";

    std::string rbuf;             // partial receive buffer
    bool eof = false;
    char sbuf[4096];              // send buffer (for partial stdin lines)
    size_t sbuf_len = 0;

    struct pollfd pfds[2];
    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;
    pfds[1].fd = fd;
    pfds[1].events = POLLIN;

    bool stdin_done = false;
    int drain_ms = 500;
    while (!eof) {
        int timeout = stdin_done ? drain_ms : -1;
        int rc = poll(pfds, 2, timeout);
        if (rc < 0) { perror("poll"); break; }
        if (stdin_done && rc == 0) break;  // drain timeout

        // ── socket readable (server response) ──
        if (pfds[1].revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) { break; }  // server disconnected
            buf[n] = 0;
            rbuf += buf;
            // print complete lines
            size_t pos;
            while ((pos = rbuf.find('\n')) != std::string::npos) {
                std::cout << "<< " << rbuf.substr(0, pos) << "\n";
                rbuf.erase(0, pos + 1);
            }
        }
        if (pfds[1].revents & (POLLHUP | POLLERR)) break;

        // ── stdin readable ──
        if (pfds[0].revents & POLLIN) {
            char buf[4096];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
            if (n <= 0) { stdin_done = true; pfds[0].fd = -1; continue; }
            // append to send buffer and process complete lines
            for (ssize_t i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') {
                    sbuf[sbuf_len] = 0;
                    std::string line(sbuf, sbuf_len);
                    sbuf_len = 0;

                    if (line.empty()) continue;
                    if (line == "quit" || line == "exit") {
                        std::string ni = "MSG Quit >> -@ Controller\n";
                        write(fd, ni.data(), ni.size());
                        stdin_done = true; pfds[0].fd = -1; break;
                    }

                    std::string ni;
                    if (line == "start")       ni = "start";
                    else if (line == "stop")   ni = "stop";
                    else if (line == "status") ni = "status";
                    else if (line == "reload") ni = "reload";
                    else if (line.substr(0, 4) == "MSG ") ni = line;
                    else                       ni = "MSG " + line;

                    std::cout << ">> " << ni << "\n";
                    ni += '\n';
                    if (write(fd, ni.data(), ni.size()) < 0) {
                        perror("write"); eof = true; break;
                    }
                } else if (c != '\r' && sbuf_len < sizeof(sbuf) - 1) {
                    sbuf[sbuf_len++] = c;
                }
            }
        }
        if (pfds[0].revents & (POLLHUP | POLLERR)) { stdin_done = true; pfds[0].fd = -1; }
    }

    // flush remaining partial line
    if (!rbuf.empty()) std::cout << "<< " << rbuf << "\n";
    close(fd);
    return 0;
}
