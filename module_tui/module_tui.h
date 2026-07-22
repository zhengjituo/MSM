#ifndef MSM_TUI_H
#define MSM_TUI_H

#include <string>
#include <deque>
#include <functional>
#include <memory>

class TUI {
public:
    using CommandCallback = std::function<void(const std::string&)>;
    using TickCallback = std::function<void()>;

    TUI();
    ~TUI();

    bool init();
    void run(CommandCallback cb, TickCallback tick = nullptr);
    void stop();

    int width() const { return cols_; }
    int height() const { return rows_; }

private:
    // ANSI helpers (write to tty_fd_)
    void ansi_clear();
    void ansi_cup(int r, int c);
    void ansi_el();
    void ansi_show_cursor(bool show);
    void ansi_write(const char* s, size_t len);

    void term_size();
    void draw();
    void drawInputLine();
    void handleInput(int ch);

    void redirectStdout();
    void restoreStdout();
    void readCapturedOutput();

    int cols_ = 80;
    int rows_ = 24;
    int inputRow_;

    int tty_fd_ = -1;
    int stdout_pipe_[2] = {-1, -1};

    std::deque<std::string> lines_;
    size_t maxLines_ = 5000;
    int scrollOffset_ = 0;

    std::string inputLine_;
    size_t cursorPos_ = 0;
    std::string prompt_ = "> ";

    CommandCallback onCommand_;
    bool running_ = false;
};

#endif
