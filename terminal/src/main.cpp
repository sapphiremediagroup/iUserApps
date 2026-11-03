// InstantOS Terminal — a PTY-backed VT/ANSI terminal emulator.
//
// Architecture:
//   * Opens /dev/ptmx to obtain a PTY master, reads the slave index via
//     TIOCGPTN, and binds the slave (/dev/pts/N) to fds 0/1/2 before spawning
//     a shell so the child gets a real controlling terminal.
//   * The window loop watches the master: whatever the shell writes is fed
//     through an ANSI/VT parser into a character-cell grid, and the rows it
//     changed are repainted.
//   * Forwards keystrokes (translating special keys to escape sequences) to the
//     master fd, where the kernel line discipline cooks them for the shell.
//   * A resize recomputes the grid and tells the shell (TIOCSWINSZ).

#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <syscall.hpp>
#include <ui/window.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr int kCursorBlinkMs = 500;
constexpr int kPaddingX = 12;
constexpr int kPaddingY = 12;
constexpr std::uint32_t kFontPixelHeight = 15;
constexpr char kFontPath[] = "/bin/JetBrainsMono-Regular.ttf";

constexpr std::uint32_t kMaxRows = 120;
constexpr std::uint32_t kMaxCols = 300;

constexpr ui::Color kColorBackground = ui::rgb(0x091218);
constexpr ui::Color kColorDefaultFg = ui::rgb(0xd7f2e6);
constexpr ui::Color kColorCursor = ui::rgb(0xf2c14e);

// The 8 ANSI base colors and their bright variants.
constexpr ui::Color kAnsiColors[16] = {
    ui::rgb(0x101010), ui::rgb(0xcc5555), ui::rgb(0x66bb66), ui::rgb(0xcccc66),
    ui::rgb(0x5588cc), ui::rgb(0xbb66bb), ui::rgb(0x66bbbb), ui::rgb(0xd7f2e6),
    ui::rgb(0x505050), ui::rgb(0xef798a), ui::rgb(0x85d39a), ui::rgb(0xf2c14e),
    ui::rgb(0x3ba4d8), ui::rgb(0xc98ad8), ui::rgb(0x66cccc), ui::rgb(0xffffff),
};

// Encoded kernel File handle for a stdio slot: (HandleType::File << 48) | slot.
// HandleType::File == 1 in the kernel handle enum.
constexpr std::uint64_t kFileHandleType = 1ULL << 48;
constexpr std::uint64_t stdio_handle(int slot) {
    return kFileHandleType | static_cast<std::uint64_t>(slot);
}

struct Cell {
    char ch;
    ui::Color fg;
    ui::Color bg;
};

struct VTParser {
    enum class State { Ground, Escape, CSI };
    State state = State::Ground;
    int params[8];
    int paramCount;
    bool paramStarted;
};

// The screen: a grid of cells, the cursor and the parser state. Every change
// widens the dirty row range, which the window turns into a repaint.
struct Terminal {
    Cell grid[kMaxRows][kMaxCols];
    std::uint32_t rows;
    std::uint32_t cols;
    std::uint32_t cursorRow;
    std::uint32_t cursorCol;
    ui::Color curFg;
    ui::Color curBg;
    VTParser parser;
    std::uint32_t dirtyTop;      // rows [dirtyTop, dirtyBottom) changed
    std::uint32_t dirtyBottom;
};

Terminal gTerminal = {};

void touch_rows(Terminal* t, std::uint32_t top, std::uint32_t bottom) {
    if (top >= bottom) {
        return;
    }
    if (t->dirtyTop >= t->dirtyBottom) {
        t->dirtyTop = top;
        t->dirtyBottom = bottom;
        return;
    }
    if (top < t->dirtyTop) t->dirtyTop = top;
    if (bottom > t->dirtyBottom) t->dirtyBottom = bottom;
}

void touch_all(Terminal* t) {
    touch_rows(t, 0, t->rows);
}

void clear_row(Terminal* t, std::uint32_t row, std::uint32_t fromCol) {
    for (std::uint32_t c = fromCol; c < t->cols; ++c) {
        t->grid[row][c].ch = ' ';
        t->grid[row][c].fg = t->curFg;
        t->grid[row][c].bg = t->curBg;
    }
    touch_rows(t, row, row + 1);
}

void clear_all(Terminal* t) {
    for (std::uint32_t r = 0; r < t->rows; ++r) {
        clear_row(t, r, 0);
    }
    t->cursorRow = 0;
    t->cursorCol = 0;
}

void scroll_up(Terminal* t) {
    for (std::uint32_t r = 1; r < t->rows; ++r) {
        for (std::uint32_t c = 0; c < t->cols; ++c) {
            t->grid[r - 1][c] = t->grid[r][c];
        }
    }
    clear_row(t, t->rows - 1, 0);
    touch_all(t);
}

void newline(Terminal* t) {
    if (t->cursorRow + 1 >= t->rows) {
        scroll_up(t);
    } else {
        t->cursorRow++;
    }
}

void put_char(Terminal* t, char ch) {
    if (t->cursorCol >= t->cols) {
        t->cursorCol = 0;
        newline(t);
    }
    Cell& cell = t->grid[t->cursorRow][t->cursorCol];
    cell.ch = ch;
    cell.fg = t->curFg;
    cell.bg = t->curBg;
    touch_rows(t, t->cursorRow, t->cursorRow + 1);
    t->cursorCol++;
}

void apply_sgr(Terminal* t, const VTParser& p) {
    if (p.paramCount == 0) {
        t->curFg = kColorDefaultFg;
        t->curBg = kColorBackground;
        return;
    }
    for (int i = 0; i < p.paramCount; ++i) {
        int code = p.params[i];
        if (code == 0) {
            t->curFg = kColorDefaultFg;
            t->curBg = kColorBackground;
        } else if (code >= 30 && code <= 37) {
            t->curFg = kAnsiColors[code - 30];
        } else if (code >= 90 && code <= 97) {
            t->curFg = kAnsiColors[(code - 90) + 8];
        } else if (code >= 40 && code <= 47) {
            t->curBg = kAnsiColors[code - 40];
        } else if (code >= 100 && code <= 107) {
            t->curBg = kAnsiColors[(code - 100) + 8];
        } else if (code == 39) {
            t->curFg = kColorDefaultFg;
        } else if (code == 49) {
            t->curBg = kColorBackground;
        }
    }
}

void handle_csi(Terminal* t, char final) {
    VTParser& p = t->parser;
    int a = (p.paramCount > 0) ? p.params[0] : 0;
    int b = (p.paramCount > 1) ? p.params[1] : 0;

    switch (final) {
        case 'A':  // cursor up
            t->cursorRow -= (a ? a : 1) <= (int)t->cursorRow ? (a ? a : 1) : t->cursorRow;
            break;
        case 'B':  // cursor down
            t->cursorRow += (a ? a : 1);
            if (t->cursorRow >= t->rows) t->cursorRow = t->rows - 1;
            break;
        case 'C':  // cursor forward
            t->cursorCol += (a ? a : 1);
            if (t->cursorCol >= t->cols) t->cursorCol = t->cols - 1;
            break;
        case 'D':  // cursor back
            t->cursorCol -= (a ? a : 1) <= (int)t->cursorCol ? (a ? a : 1) : t->cursorCol;
            break;
        case 'H':
        case 'f': {  // cursor position (1-based)
            std::uint32_t row = a > 0 ? (std::uint32_t)(a - 1) : 0;
            std::uint32_t col = b > 0 ? (std::uint32_t)(b - 1) : 0;
            t->cursorRow = row < t->rows ? row : t->rows - 1;
            t->cursorCol = col < t->cols ? col : t->cols - 1;
            break;
        }
        case 'J':  // erase display
            if (a == 2 || a == 3) {
                clear_all(t);
            } else if (a == 0) {
                clear_row(t, t->cursorRow, t->cursorCol);
                for (std::uint32_t r = t->cursorRow + 1; r < t->rows; ++r) clear_row(t, r, 0);
            } else if (a == 1) {
                for (std::uint32_t r = 0; r < t->cursorRow; ++r) clear_row(t, r, 0);
                clear_row(t, t->cursorRow, 0);
            }
            break;
        case 'K':  // erase line
            if (a == 0) {
                clear_row(t, t->cursorRow, t->cursorCol);
            } else if (a == 1) {
                for (std::uint32_t c = 0; c <= t->cursorCol && c < t->cols; ++c) {
                    t->grid[t->cursorRow][c].ch = ' ';
                }
                touch_rows(t, t->cursorRow, t->cursorRow + 1);
            } else if (a == 2) {
                clear_row(t, t->cursorRow, 0);
            }
            break;
        case 'm':
            apply_sgr(t, p);
            break;
        default:
            break;
    }
}

void parser_feed(Terminal* t, char ch) {
    VTParser& p = t->parser;
    switch (p.state) {
        case VTParser::State::Ground:
            if (ch == 0x1B) {
                p.state = VTParser::State::Escape;
            } else if (ch == '\n') {
                newline(t);
            } else if (ch == '\r') {
                t->cursorCol = 0;
            } else if (ch == '\b') {
                if (t->cursorCol > 0) t->cursorCol--;
            } else if (ch == '\t') {
                std::uint32_t next = (t->cursorCol + 8) & ~7u;
                while (t->cursorCol < next && t->cursorCol < t->cols) put_char(t, ' ');
            } else if (ch == 7) {
                // bell: ignore
            } else if ((unsigned char)ch >= 32) {
                put_char(t, ch);
            }
            break;
        case VTParser::State::Escape:
            if (ch == '[') {
                p.state = VTParser::State::CSI;
                p.paramCount = 0;
                p.paramStarted = false;
                for (int i = 0; i < 8; ++i) p.params[i] = 0;
            } else {
                p.state = VTParser::State::Ground;
            }
            break;
        case VTParser::State::CSI:
            if (ch >= '0' && ch <= '9') {
                if (p.paramCount == 0) p.paramCount = 1;
                p.params[p.paramCount - 1] = p.params[p.paramCount - 1] * 10 + (ch - '0');
                p.paramStarted = true;
            } else if (ch == ';') {
                if (p.paramCount < 8) p.paramCount++;
                if (p.paramCount > 0 && p.paramCount <= 8) p.params[p.paramCount - 1] = 0;
            } else if (ch == '?') {
                // private mode introducer; ignore
            } else if (ch >= 0x40 && ch <= 0x7E) {
                handle_csi(t, ch);
                p.state = VTParser::State::Ground;
            } else {
                p.state = VTParser::State::Ground;
            }
            break;
    }
}

void feed_bytes(Terminal* t, const char* data, std::uint64_t len) {
    for (std::uint64_t i = 0; i < len; ++i) {
        parser_feed(t, data[i]);
    }
}

// Rows and columns that fit `size`.
void fit_grid(Terminal* t, ui::Size size, int cellW, int lineH) {
    const int cols = (size.width - kPaddingX * 2) / cellW;
    const int rows = (size.height - kPaddingY * 2) / lineH;
    const std::uint32_t newCols = cols < 1 ? 1 : (cols > (int)kMaxCols ? kMaxCols : (std::uint32_t)cols);
    const std::uint32_t newRows = rows < 1 ? 1 : (rows > (int)kMaxRows ? kMaxRows : (std::uint32_t)rows);
    // Cells that come into view for the first time start blank.
    for (std::uint32_t r = 0; r < newRows; ++r) {
        for (std::uint32_t c = (r < t->rows ? t->cols : 0); c < newCols; ++c) {
            t->grid[r][c] = { ' ', kColorDefaultFg, kColorBackground };
        }
    }
    // Shrinking below the cursor scrolls the screen up so it stays in view.
    t->cols = newCols;
    while (t->cursorRow >= newRows) {
        t->rows = t->cursorRow + 1;
        scroll_up(t);
        t->cursorRow--;
    }
    t->rows = newRows;
    if (t->cursorCol > t->cols) {
        t->cursorCol = t->cols;
    }
    touch_all(t);
}

// ---- input ----------------------------------------------------------------

// The xterm sequence a key sends, or nullptr for keys that are not one.
const char* key_sequence(std::uint16_t code) {
    switch (code) {
        case std::KeyUp: return "\x1b[A";
        case std::KeyDown: return "\x1b[B";
        case std::KeyRight: return "\x1b[C";
        case std::KeyLeft: return "\x1b[D";
        case std::KeyHome: return "\x1b[H";
        case std::KeyEnd: return "\x1b[F";
        case std::KeyInsert: return "\x1b[2~";
        case std::KeyDelete: return "\x1b[3~";
        case std::KeyPageUp: return "\x1b[5~";
        case std::KeyPageDown: return "\x1b[6~";
        case std::KeyF1: return "\x1bOP";
        case std::KeyF2: return "\x1bOQ";
        case std::KeyF3: return "\x1bOR";
        case std::KeyF4: return "\x1bOS";
        case std::KeyF5: return "\x1b[15~";
        case std::KeyF6: return "\x1b[17~";
        case std::KeyF7: return "\x1b[18~";
        case std::KeyF8: return "\x1b[19~";
        case std::KeyF9: return "\x1b[20~";
        case std::KeyF10: return "\x1b[21~";
        case std::KeyF11: return "\x1b[23~";
        case std::KeyF12: return "\x1b[24~";
        default: return nullptr;
    }
}

// Translate a key press into bytes to write to the PTY master. Returns the
// number of bytes written into out (up to 8), or 0 if the key is ignored.
int translate_key(const ui::KeyEvent& key, char* out) {
    const std::uint16_t code = key.key;
    const bool ctrl = key.ctrl();
    const bool alt = key.alt();

    if (const char* sequence = key_sequence(code)) {
        int n = 0;
        while (sequence[n] != '\0') { out[n] = sequence[n]; ++n; }
        // Modifiers go in as xterm's parameter, 1 + Shift 1, Alt 2, Ctrl 4:
        // ESC [ A becomes ESC [ 1 ; 5 A, ESC O P ESC [ 1 ; 5 P, ESC [ 3 ~
        // ESC [ 3 ; 5 ~ (the kernel's console does the same).
        const char param = (char)('1' + (key.shift() ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0));
        if (param != '1') {
            const char final = out[n - 1];
            n = (final == '~') ? n - 1 : 2;
            out[1] = '[';
            if (final != '~') { out[n++] = '1'; }
            out[n++] = ';';
            out[n++] = param;
            out[n++] = final;
        }
        return n;
    }

    // Alt sends ESC before the key, as xterm's metaSendsEscape.
    int n = 0;
    if (alt) { out[n++] = 0x1B; }

    switch (code) {
        case std::KeyEnter: case std::KeypadEnter: out[n++] = '\n'; return n;
        case std::KeyBackspace: out[n++] = 0x7F; return n;   // DEL, as xterm
        case std::KeyTab: out[n++] = '\t'; return n;
        case std::KeyEscape: out[n++] = 0x1B; return n;
        default: break;
    }

    const char ch = key.text[0];
    if (ctrl) {
        // Ctrl-A..Ctrl-Z -> 1..26, plus the other C0 controls on their keys.
        if (code >= std::KeyA && code <= std::KeyZ) { out[n++] = (char)(code - std::KeyA + 1); return n; }
        switch (code) {
            case std::KeySpace: case std::Key2: out[n++] = 0x00; return n;     // Ctrl-@
            case std::KeyLeftBracket: out[n++] = 0x1B; return n;               // Ctrl-[
            case std::KeyBackslash: out[n++] = 0x1C; return n;                 // Ctrl-backslash
            case std::KeyRightBracket: out[n++] = 0x1D; return n;              // Ctrl-]
            case std::Key6: out[n++] = 0x1E; return n;                         // Ctrl-^
            case std::KeyMinus: case std::KeySlash: out[n++] = 0x1F; return n; // Ctrl-_
            default: break;
        }
    }
    if (ch == '\0') {
        return 0;
    }
    out[n++] = ch;
    return n;
}

// ---- shell spawning --------------------------------------------------------

const char* kShellCandidates[] = {
    "/bin/bash",
    "/bin/sh",
    "/bin/dash",
    nullptr,
};

std::uint64_t spawn_shell(std::Handle slaveFd) {
    // Bind the slave to stdio slots so the spawned child inherits them. The
    // slave is a full 64-bit encoded handle; it must not be truncated to int.
    std::dup2(slaveFd, stdio_handle(0));
    std::dup2(slaveFd, stdio_handle(1));
    std::dup2(slaveFd, stdio_handle(2));

    std::uint64_t pid = fail;
    for (int i = 0; kShellCandidates[i] != nullptr; ++i) {
        pid = std::spawn(kShellCandidates[i]);
        if (pid != fail) {
            break;
        }
    }
    return pid;
}

void build_pts_path(char* out, std::uint32_t n) {
    const char* prefix = "/dev/pts/";
    int i = 0;
    while (prefix[i]) { out[i] = prefix[i]; i++; }
    char digits[12];
    int d = 0;
    if (n == 0) { digits[d++] = '0'; }
    while (n > 0) { digits[d++] = (char)('0' + (n % 10)); n /= 10; }
    while (d > 0) { out[i++] = digits[--d]; }
    out[i] = '\0';
}

class TerminalWindow : public ui::Window {
    Terminal* t_ = &gTerminal;
    ui::Font font_;
    int cellW_ = 8;
    int lineH_ = 16;
    std::Handle master_ = fail;
    bool cursorShown_ = true;
    int blinkTimer_ = 0;
    // Where the cursor was last painted, so moving it repaints the old row.
    std::uint32_t paintedCursorRow_ = 0;

    void on_create() override {
        if (font_.load(kFontPath, kFontPixelHeight)) {
            cellW_ = font_.width("M");
            lineH_ = font_.line_height();
        }
        t_->curFg = kColorDefaultFg;
        t_->curBg = kColorBackground;
        t_->rows = 0;
        t_->cols = 0;
        fit_grid(t_, client_size(), cellW_, lineH_);

        if (!open_pty()) {
            const char* message = "terminal: could not start a shell\r\n";
            feed_bytes(t_, message, std::strlen(message));
        }
        blinkTimer_ = set_timer(kCursorBlinkMs);
        flush();
    }

    void on_size(ui::Size size) override {
        fit_grid(t_, size, cellW_, lineH_);
        set_shell_size();
    }

    void on_destroy() override {
        if (master_ != fail) {
            std::close(master_);
            master_ = fail;
        }
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect& dirty) override {
        canvas.fill(dirty, kColorBackground);
        const ui::Font& font = font_.valid() ? font_ : ui::Font::ui();
        const int ascent = font.ascent();
        int firstRow = (dirty.y - kPaddingY) / lineH_;
        if (firstRow < 0) firstRow = 0;
        const int lastRow = (dirty.bottom() - kPaddingY + lineH_ - 1) / lineH_;
        char glyph[2] = { 0, 0 };

        for (int r = firstRow; r < lastRow && r < (int)t_->rows; ++r) {
            const int y = kPaddingY + r * lineH_;
            for (std::uint32_t c = 0; c < t_->cols; ++c) {
                const Cell& cell = t_->grid[r][c];
                const int x = kPaddingX + (int)c * cellW_;
                if (!(cell.bg == kColorBackground)) {
                    canvas.fill({ x, y, cellW_, lineH_ }, cell.bg);
                }
                if (cell.ch != ' ' && cell.ch != 0) {
                    glyph[0] = cell.ch;
                    canvas.text(x, y + ascent, glyph, font, cell.fg);
                }
            }
        }

        // The cursor: a block while the window has the focus (blinking), an
        // outline while it does not.
        const int x = kPaddingX + (int)t_->cursorCol * cellW_;
        const int y = kPaddingY + (int)t_->cursorRow * lineH_;
        if (!focused()) {
            canvas.frame({ x, y, cellW_, lineH_ }, kColorCursor);
        } else if (cursorShown_) {
            canvas.fill({ x, y, cellW_, lineH_ }, kColorCursor);
            if (t_->cursorCol < t_->cols) {
                const Cell& cell = t_->grid[t_->cursorRow][t_->cursorCol];
                if (cell.ch != ' ' && cell.ch != 0) {
                    glyph[0] = cell.ch;
                    canvas.text(x, y + ascent, glyph, font, kColorBackground);
                }
            }
        }
        paintedCursorRow_ = t_->cursorRow;
    }

    bool on_key_down(const ui::KeyEvent& key) override {
        char seq[8];
        const int n = translate_key(key, seq);
        if (n > 0 && master_ != fail) {
            std::write(master_, seq, (std::uint64_t)n);
            restart_blink();
        }
        // Every key belongs to the shell (Tab included).
        return true;
    }

    void on_readable(std::Handle handle) override {
        char buf[512];
        // A few reads at most, so a shell that prints without pause still
        // lets the window repaint and take keys.
        for (int i = 0; i < 32; ++i) {
            std::PollFD pfd = { (std::int64_t)handle, 0x0001, 0 };
            if (i > 0 && (std::poll(&pfd, 1, 0) == fail || (pfd.revents & 0x0001) == 0)) {
                break;
            }
            const std::uint64_t n = std::read(handle, buf, sizeof(buf));
            if (n == fail || n == 0) {
                // The shell is gone: say so, and stop watching a master
                // that will now always read as ready.
                if (i == 0) {
                    unwatch(handle);
                    const char* message = "\r\n[shell exited]";
                    feed_bytes(t_, message, std::strlen(message));
                }
                break;
            }
            feed_bytes(t_, buf, n);
        }
        restart_blink();
        flush();
    }

    void on_timer(int id) override {
        if (id == blinkTimer_) {
            cursorShown_ = !cursorShown_;
            touch_rows(t_, t_->cursorRow, t_->cursorRow + 1);
            flush();
        }
    }

    void on_focus(bool) override {
        restart_blink();
        flush();
    }

    void restart_blink() {
        cursorShown_ = true;
        touch_rows(t_, t_->cursorRow, t_->cursorRow + 1);
        if (blinkTimer_ != 0) {
            kill_timer(blinkTimer_);
        }
        blinkTimer_ = set_timer(kCursorBlinkMs);
    }

    // Repaints the rows the screen changed, and the cursor's old row.
    void flush() {
        touch_rows(t_, paintedCursorRow_, paintedCursorRow_ + 1);
        if (t_->dirtyTop < t_->dirtyBottom) {
            const int top = kPaddingY + (int)t_->dirtyTop * lineH_;
            const int bottom = kPaddingY + (int)t_->dirtyBottom * lineH_;
            invalidate({ 0, top, client_size().width, bottom - top });
        }
        t_->dirtyTop = t_->dirtyBottom = 0;
    }

    void set_shell_size() {
        if (master_ == fail) {
            return;
        }
        std::Winsize ws = {};
        ws.ws_row = (std::uint16_t)t_->rows;
        ws.ws_col = (std::uint16_t)t_->cols;
        std::ioctl(master_, std::TIOCSWINSZ, reinterpret_cast<std::uint64_t>(&ws));
    }

    bool open_pty() {
        std::Handle master = std::open("/dev/ptmx", O_RDWR);
        if (master == fail) {
            return false;
        }
        master_ = master;
        // Set the window size so the shell knows the terminal geometry.
        set_shell_size();

        // Resolve the slave index and open /dev/pts/N.
        std::uint32_t ptn = 0;
        if (std::ioctl(master, std::TIOCGPTN, reinterpret_cast<std::uint64_t>(&ptn)) == fail) {
            return false;
        }
        char slavePath[32];
        build_pts_path(slavePath, ptn);
        std::Handle slave = std::open(slavePath, O_RDWR);
        if (slave == fail) {
            return false;
        }

        const std::uint64_t pid = spawn_shell(slave);
        std::close(slave);
        if (pid == fail) {
            return false;
        }
        // The shell is the terminal's foreground job (each process is its own
        // group here): bash with job control waits until it is.
        std::uint32_t pgrp = static_cast<std::uint32_t>(pid);
        std::ioctl(master, std::TIOCSPGRP, reinterpret_cast<std::uint64_t>(&pgrp));
        watch(master);
        return true;
    }
};

}  // namespace

int main(int, char**) {
    return ui::run<TerminalWindow>({ .width = 920, .height = 560, .title = "Terminal" });
}
