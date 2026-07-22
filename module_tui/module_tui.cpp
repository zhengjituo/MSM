#include "module_tui.h"
#include <iostream>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <termios.h>
#include <sys/ioctl.h>

static struct termios g_orig_term;

static void term_raw() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_term);
    raw = g_orig_term;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void term_restore() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_term);
}

// ── ANSI helpers (use tty_fd_ directly) ──

void TUI::ansi_clear() { write(tty_fd_, "\033[2J\033[H", 7); }
void TUI::ansi_cup(int r, int c) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\033[%d;%dH", r, c);
    write(tty_fd_, buf, n);
}
void TUI::ansi_el() { write(tty_fd_, "\033[K", 3); }
void TUI::ansi_show_cursor(bool show) {
    write(tty_fd_, show ? "\033[?25h" : "\033[?25l", 6);
}
void TUI::ansi_write(const char* s, size_t len) { write(tty_fd_, s, len); }

TUI::TUI() {}
TUI::~TUI() { stop(); }

bool TUI::init() {
    // save terminal fd before redirect
    tty_fd_ = dup(STDOUT_FILENO);
    if (tty_fd_ < 0) return false;

    term_size();
    term_raw();
    redirectStdout();
    ansi_clear();
    ansi_show_cursor(false);
    return true;
}

void TUI::term_size() {
    struct winsize ws;
    if (ioctl(tty_fd_, TIOCGWINSZ, &ws) == 0) {
        cols_ = ws.ws_col;
        rows_ = ws.ws_row;
    } else {
        cols_ = 80; rows_ = 24;
    }
}

void TUI::stop() {
    if (running_) {
        running_ = false;
        restoreStdout();
        ansi_show_cursor(true);
        ansi_clear();
        term_restore();
    }
    if (tty_fd_ >= 0) { close(tty_fd_); tty_fd_ = -1; }
}

void TUI::redirectStdout() {
    if (pipe(stdout_pipe_) < 0) return;
    fcntl(stdout_pipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(stdout_pipe_[1], F_SETFL, O_NONBLOCK);
    dup2(stdout_pipe_[1], STDOUT_FILENO);
    dup2(stdout_pipe_[1], STDERR_FILENO);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

void TUI::restoreStdout() {
    if (tty_fd_ >= 0) {
        dup2(tty_fd_, STDOUT_FILENO);
        dup2(tty_fd_, STDERR_FILENO);
    }
    if (stdout_pipe_[0] >= 0) close(stdout_pipe_[0]);
    if (stdout_pipe_[1] >= 0) close(stdout_pipe_[1]);
    stdout_pipe_[0] = stdout_pipe_[1] = -1;
}

void TUI::readCapturedOutput() {
    char buf[4096];
    ssize_t n;
    while ((n = read(stdout_pipe_[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        size_t start = 0;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                lines_.push_back(std::string(buf + start));
                start = i + 1;
            }
        }
        if (start < (size_t)n) {
            // partial line — ignore
        }
    }
    while (lines_.size() > maxLines_)
        lines_.pop_front();
}

void TUI::draw() {
    term_size();
    int outRows = rows_ - 2;  // leave 1 divider + 1 input line

    // ── Output area ──
    readCapturedOutput();
    size_t total = lines_.size();
    size_t start = (total > (size_t)outRows) ? total - outRows : 0;
    ansi_cup(1, 1);
    for (size_t i = start; i < total; i++) {
        ansi_el();
        size_t len = lines_[i].size();
        if (len > (size_t)cols_ - 1) len = (size_t)cols_ - 1;
        ansi_write(lines_[i].data(), len);
        ansi_write("\n", 1);
    }
    // clear remaining lines in output area
    for (size_t i = total - start; i < (size_t)outRows; i++) {
        ansi_el();
        ansi_write("\n", 1);
    }

    // ── Divider ──
    ansi_cup(outRows + 1, 1);
    ansi_el();
    for (int c = 0; c < cols_; c++)
        ansi_write("\u2500", 3);

    // ── Input line ──
    drawInputLine();
}

void TUI::drawInputLine() {
    ansi_cup(rows_, 1);
    ansi_el();
    ansi_write(prompt_.data(), prompt_.size());
    ansi_write(inputLine_.data(), inputLine_.size());
    int cx = (int)(prompt_.size() + cursorPos_) + 1;
    if (cx > cols_) cx = cols_;
    ansi_cup(rows_, cx);
}

void TUI::handleInput(int ch) {
    if (ch == '\n' || ch == '\r') {
        std::string cmd = inputLine_;
        inputLine_.clear();
        cursorPos_ = 0;
        if (onCommand_ && !cmd.empty())
            onCommand_(cmd);
        return;
    }
    if (ch == 127 || ch == '\b') {
        if (cursorPos_ > 0) {
            inputLine_.erase(cursorPos_ - 1, 1);
            cursorPos_--;
        }
        return;
    }
    if (ch == '\033') {
        char seq[2];
        if (read(STDIN_FILENO, &seq, 2) == 2) {
            if (seq[0] == '[') {
                if (seq[1] == 'C' && cursorPos_ < inputLine_.size()) cursorPos_++;
                else if (seq[1] == 'D' && cursorPos_ > 0) cursorPos_--;
            }
        }
        return;
    }
    if (ch >= 32 && ch <= 126 && inputLine_.size() < 512) {
        inputLine_.insert(cursorPos_, 1, (char)ch);
        cursorPos_++;
    }
}

void TUI::run(CommandCallback cb, TickCallback tick) {
    onCommand_ = std::move(cb);
    running_ = true;
    draw();

    struct pollfd pfds[2];
    pfds[0].fd = STDIN_FILENO;
    pfds[0].events = POLLIN;
    pfds[1].fd = stdout_pipe_[0];
    pfds[1].events = POLLIN;

    while (running_) {
        int rc = poll(pfds, 2, 100);
        if (rc < 0) break;

        if (pfds[1].revents & POLLIN)
            readCapturedOutput();

        if (pfds[0].revents & POLLIN) {
            char ch;
            if (read(STDIN_FILENO, &ch, 1) != 1) break;
            handleInput(ch);
        }

        if (tick) tick();
        if (!running_) break;

        draw();
    }

    stop();
}
