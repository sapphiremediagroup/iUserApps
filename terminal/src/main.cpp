// InstantOS Terminal — a PTY-backed VT/ANSI terminal emulator.
//
// Architecture:
//   * Opens /dev/ptmx to obtain a PTY master, reads the slave index via
//     TIOCGPTN, and binds the slave (/dev/pts/N) to fds 0/1/2 before spawning
//     a shell so the child gets a real controlling terminal.
//   * Reads bytes from the master, feeds them through an ANSI/VT parser into a
//     character-cell grid, and renders the grid to the window surface.
//   * Forwards keystrokes (translating special keys to escape sequences) to the
//     master fd, where the kernel line discipline cooks them for the shell.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <instant/font.hpp>
#include <instant/window.hpp>
#include <new.hpp>
#include <syscall.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kSurfaceWidth = 920;
constexpr std::uint32_t kSurfaceHeight = 560;
constexpr std::uint32_t kFrameIntervalMs = 16;
constexpr std::uint32_t kCursorBlinkMs = 500;
constexpr int kPaddingX = 12;
constexpr int kPaddingY = 12;
constexpr std::uint32_t kFontPixelHeight = 15;

constexpr std::uint32_t kMaxRows = 60;
constexpr std::uint32_t kMaxCols = 200;

constexpr std::uint32_t kColorBackground = 0x00091218U;
constexpr std::uint32_t kColorDefaultFg = 0x00d7f2e6U;
constexpr std::uint32_t kColorCursor = 0x00f2c14eU;

// The 8 ANSI base colors and their bright variants (0x00RRGGBB).
constexpr std::uint32_t kAnsiColors[16] = {
    0x00101010, 0x00cc5555, 0x0066bb66, 0x00cccc66,
    0x005588cc, 0x00bb66bb, 0x0066bbbb, 0x00d7f2e6,
    0x00505050, 0x00ef798a, 0x0085d39a, 0x00f2c14e,
    0x003ba4d8, 0x00c98ad8, 0x0066cccc, 0x00ffffff,
};

// Encoded kernel File handle for a stdio slot: (HandleType::File << 48) | slot.
// HandleType::File == 1 in the kernel handle enum.
constexpr std::uint64_t kFileHandleType = 1ULL << 48;
constexpr std::uint64_t stdio_handle(int slot) {
    return kFileHandleType | static_cast<std::uint64_t>(slot);
}

struct Cell {
    char ch;
    std::uint32_t fg;
    std::uint32_t bg;
};

struct VTParser {
    enum class State { Ground, Escape, CSI };
    State state = State::Ground;
    int params[8];
    int paramCount;
    bool paramStarted;
};

struct Terminal {
    Cell grid[kMaxRows][kMaxCols];
    std::uint32_t rows;
    std::uint32_t cols;
    std::uint32_t cursorRow;
    std::uint32_t cursorCol;
    std::uint32_t curFg;
    std::uint32_t curBg;
    bool focused;
    bool running;
    bool cursorVisible;
    std::uint64_t lastBlinkMs;

    std::Handle masterFd;
    std::uint64_t childPid;

    VTParser parser;
};

static Terminal gTerminal = {};

void clear_row(Terminal* t, std::uint32_t row, std::uint32_t fromCol) {
    for (std::uint32_t c = fromCol; c < t->cols; ++c) {
        t->grid[row][c].ch = ' ';
        t->grid[row][c].fg = t->curFg;
        t->grid[row][c].bg = t->curBg;
    }
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

// ---- rendering ------------------------------------------------------------

void fill_rect(std::uint32_t* px, std::uint32_t pitch, std::uint32_t W, std::uint32_t H,
               int x, int y, int w, int h, std::uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > (int)W) x1 = (int)W;
    int y1 = y + h; if (y1 > (int)H) y1 = (int)H;
    for (int row = y0; row < y1; ++row) {
        for (int col = x0; col < x1; ++col) {
            px[row * pitch + col] = color;
        }
    }
}

void draw_terminal(std::uint32_t* px, std::uint32_t pitch, std::uint32_t W, std::uint32_t H,
                   instant::UIFont& font, Terminal* t) {
    fill_rect(px, pitch, W, H, 0, 0, (int)W, (int)H, kColorBackground);

    const int cellW = font.cellWidth;
    const int lineH = font.lineHeight;
    char glyph[2] = { 0, 0 };

    for (std::uint32_t r = 0; r < t->rows; ++r) {
        int baseY = kPaddingY + (int)r * lineH + font.baseline;
        for (std::uint32_t c = 0; c < t->cols; ++c) {
            const Cell& cell = t->grid[r][c];
            int x = kPaddingX + (int)c * cellW;
            if (cell.bg != kColorBackground) {
                fill_rect(px, pitch, W, H, x, kPaddingY + (int)r * lineH, cellW, lineH, cell.bg);
            }
            if (cell.ch != ' ' && cell.ch != 0) {
                glyph[0] = cell.ch;
                instant::draw_text(px, W, H, font, x, baseY, glyph, cell.fg);
            }
        }
    }

    if (t->cursorVisible && t->focused) {
        int x = kPaddingX + (int)t->cursorCol * cellW;
        int y = kPaddingY + (int)t->cursorRow * lineH;
        fill_rect(px, pitch, W, H, x, y, cellW, lineH, kColorCursor);
        if (t->cursorCol < t->cols) {
            const Cell& cell = t->grid[t->cursorRow][t->cursorCol];
            if (cell.ch != ' ' && cell.ch != 0) {
                glyph[0] = cell.ch;
                instant::draw_text(px, W, H, font, x, y + font.baseline, glyph, kColorBackground);
            }
        }
    }
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

// Translate a key event into bytes to write to the PTY master. Returns the
// number of bytes written into out (up to 8), or 0 if the key is ignored.
int translate_key(const std::Event& event, char* out) {
    if (event.type != std::EventType::Key || event.key.action == std::KeyEventAction::Release) {
        return 0;
    }

    // Super combinations are the desktop's shortcuts (Super+T, ...).
    if ((event.key.modifiers & std::KeyModifierSuper) != 0) {
        return 0;
    }

    const std::uint16_t code = event.key.keycode;
    const bool ctrl = (event.key.modifiers & std::KeyModifierControl) != 0;
    const bool alt = (event.key.modifiers & std::KeyModifierAlt) != 0;

    if (const char* sequence = key_sequence(code)) {
        int n = 0;
        while (sequence[n] != '\0') { out[n] = sequence[n]; ++n; }
        // Modifiers go in as xterm's parameter, 1 + Shift 1, Alt 2, Ctrl 4:
        // ESC [ A becomes ESC [ 1 ; 5 A, ESC O P ESC [ 1 ; 5 P, ESC [ 3 ~
        // ESC [ 3 ; 5 ~ (the kernel's console does the same).
        const bool shift = (event.key.modifiers & std::KeyModifierShift) != 0;
        const char param = (char)('1' + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0));
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

    const char ch = event.key.text[0];
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

}  // namespace

class TerminalWindow : public instant::Window {
private:
    instant::WindowConfig configure() override {
        instant::WindowConfig config = {};
        config.width = static_cast<int>(kSurfaceWidth);
        config.height = static_cast<int>(kSurfaceHeight);
        config.title = "Terminal";
        config.frameIntervalMs = kFrameIntervalMs;
        config.autoCommit = false;
        return config;
    }

    Result<bool, std::string> init() override {
        if (!instant::initialize_ui_font(kFontPixelHeight)) {
            return Result<bool, std::string>::error("font init failed");
        }

        t_ = &gTerminal;
        std::memset(t_, 0, sizeof(*t_));
        t_->running = true;
        t_->focused = true;
        t_->cursorVisible = true;
        t_->curFg = kColorDefaultFg;
        t_->curBg = kColorBackground;
        t_->masterFd = fail;
        t_->childPid = fail;

        const int cellW = instant::gUIFont.cellWidth > 0 ? instant::gUIFont.cellWidth : 8;
        const int lineH = instant::gUIFont.lineHeight > 0 ? instant::gUIFont.lineHeight : 16;
        t_->cols = (kSurfaceWidth - (kPaddingX * 2)) / (std::uint32_t)cellW;
        t_->rows = (kSurfaceHeight - (kPaddingY * 2)) / (std::uint32_t)lineH;
        if (t_->cols > kMaxCols) t_->cols = kMaxCols;
        if (t_->rows > kMaxRows) t_->rows = kMaxRows;
        clear_all(t_);

        if (!open_pty()) {
            return Result<bool, std::string>::error("pty open failed");
        }

        redraw_ = true;
        return true;
    }

    bool open_pty() {
        std::Handle master = std::open("/dev/ptmx", O_RDWR);
        if (master == fail) {
            return false;
        }
        t_->masterFd = master;

        // Set the window size so the shell knows the terminal geometry.
        std::Winsize ws = {};
        ws.ws_row = (std::uint16_t)t_->rows;
        ws.ws_col = (std::uint16_t)t_->cols;
        std::ioctl(master, std::TIOCSWINSZ, reinterpret_cast<std::uint64_t>(&ws));

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

        t_->childPid = spawn_shell(slave);
        std::close(slave);
        // The shell is the terminal's foreground job (each process is its own
        // group here): bash with job control waits until it is.
        if (t_->childPid != fail) {
            std::uint32_t pgrp = static_cast<std::uint32_t>(t_->childPid);
            std::ioctl(master, std::TIOCSPGRP, reinterpret_cast<std::uint64_t>(&pgrp));
        }
        // Restore our own stdio slots to avoid keeping the slave bound here.
        return t_->childPid != fail;
    }

    static void build_pts_path(char* out, std::uint32_t n) {
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

    Result<bool, std::string> update() override {
        if (!t_) {
            return true;
        }

        // Drain available output from the master without blocking forever.
        if (t_->masterFd != fail) {
            std::PollFD pfd = {};
            pfd.fd = (std::int64_t)t_->masterFd;
            pfd.events = 0x0001;  // POLLIN
            char buf[512];
            for (int iter = 0; iter < 32; ++iter) {
                pfd.revents = 0;
                std::poll(&pfd, 1);
                if ((pfd.revents & 0x0001) == 0) {
                    break;
                }
                std::uint64_t n = std::read(t_->masterFd, buf, sizeof(buf));
                if (n == fail || n == 0) {
                    break;
                }
                feed_bytes(t_, buf, n);
                redraw_ = true;
            }
        }

        const std::uint64_t now = std::gettime();
        if (now - t_->lastBlinkMs >= kCursorBlinkMs) {
            t_->cursorVisible = !t_->cursorVisible;
            t_->lastBlinkMs = now;
            redraw_ = true;
        }

        if (redraw_) {
            draw_terminal(pixels(), (std::uint32_t)pitch(), (std::uint32_t)width(),
                          (std::uint32_t)height(), instant::gUIFont, t_);
            if (!commit()) {
                return Result<bool, std::string>::error("surface_commit failed");
            }
            redraw_ = false;
        }

        return t_->running;
    }

    Result<bool, std::string> event(const std::Event& event) override {
        if (!t_) {
            return true;
        }

        if (event.type == std::EventType::Window) {
            if (event.window.action == std::WindowEventAction::FocusGained) {
                t_->focused = true;
                redraw_ = true;
            } else if (event.window.action == std::WindowEventAction::FocusLost) {
                t_->focused = false;
                redraw_ = true;
            } else if (event.window.action == std::WindowEventAction::CloseRequested) {
                t_->running = false;
                return false;
            }
            return t_->running;
        }

        char seq[8];
        int n = translate_key(event, seq);
        if (n > 0 && t_->masterFd != fail) {
            std::write(t_->masterFd, seq, (std::uint64_t)n);
            t_->cursorVisible = true;
            t_->lastBlinkMs = std::gettime();
            redraw_ = true;
        }

        return t_->running;
    }

    Result<bool, std::string> event() override {
        return true;
    }

    void cleanup() override {
        instant::destroy_ui_font();
        if (t_ && t_->masterFd != fail) {
            std::close(t_->masterFd);
            t_->masterFd = fail;
        }
    }

    Terminal* t_ = nullptr;
    bool redraw_ = false;
};

INSTANT_WINDOW_APP(TerminalWindow)
