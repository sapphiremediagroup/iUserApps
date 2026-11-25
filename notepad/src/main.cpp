// Notepad: a text editor on the UI library's TextEdit, with syntax colours by
// the file's kind, Open/Save sheets and search.
//
//   notepad [FILE]      (a FILE that does not exist yet is made on Save)
//
// Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Shift+S save as, Ctrl+F search
// (Enter or F3 the next match, Shift+F3 the previous, Escape back to the
// text). Closing with changes not saved asks first.

#include <cstdint.hpp>
#include <cstring.hpp>
#include <fcntl.h>
#include <memory.hpp>
#include <syscall.hpp>
#include <ui/ui.hpp>

namespace {

constexpr std::uint64_t kFail = static_cast<std::uint64_t>(-1);
constexpr std::uint64_t kMaxFile = 32ull << 20;   // what Notepad opens at most
constexpr int kButton = 28;
constexpr int kSearchWidth = 220;
constexpr int kIconSize = 16;

const char* gStartPath = nullptr;   // argv[1]

void copy_text(char* out, std::size_t size, const char* text) {
    std::size_t i = 0;
    for (; text && text[i] && i + 1 < size; ++i) {
        out[i] = text[i];
    }
    out[i] = '\0';
}

void append_text(char* out, std::size_t size, const char* text) {
    std::size_t at = std::strlen(out);
    for (std::size_t i = 0; text[i] && at + 1 < size; ++i) {
        out[at++] = text[i];
    }
    out[at] = '\0';
}

void append_int(char* out, std::size_t size, std::uint64_t value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value > 0);
    char text[24];
    for (int i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = '\0';
    append_text(out, size, text);
}

const char* base_name(const char* path) {
    const char* base = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '/') {
            base = c + 1;
        }
    }
    return base;
}

bool is_folder(const char* path) {
    std::Stat st = {};
    return std::stat(path, &st) != kFail && (st.st_mode & 0170000) == 0040000;
}

bool load_icon(ui::Image& image, const char* name, int size, ui::Color color) {
    char path[64] = "/bin/icons/";
    append_text(path, sizeof(path), name);
    append_text(path, sizeof(path), ".svg");
    if (!image.load_svg(path, size, size)) {
        return false;
    }
    image.tint(color);
    return true;
}

// The whole file, NUL-terminated, in `*data` (to free). 0 when it was read;
// otherwise 1 missing, 2 too big, 3 unreadable.
int read_file(const char* path, char** data, std::size_t* length) {
    std::Stat st = {};
    if (std::stat(path, &st) == kFail) {
        return 1;
    }
    if (st.st_size > kMaxFile) {
        return 2;
    }
    const std::FileHandle file = std::open(path, O_RDONLY);
    if (file == kFail) {
        return 3;
    }
    std::size_t capacity = static_cast<std::size_t>(st.st_size) + 1;
    char* buffer = static_cast<char*>(std::malloc(capacity));
    std::size_t size = 0;
    bool ok = buffer != nullptr;
    while (ok) {
        if (size + 1 >= capacity) {
            // The file grew (or stat had no size): read on.
            capacity = capacity * 2 + 4096;
            if (capacity > kMaxFile + 1) {
                ok = false;
                break;
            }
            char* grown = static_cast<char*>(std::realloc(buffer, capacity));
            if (!grown) {
                ok = false;
                break;
            }
            buffer = grown;
        }
        const std::uint64_t got = std::read(file, buffer + size, capacity - 1 - size);
        if (got == kFail) {
            ok = false;
        } else if (got == 0) {
            break;
        } else {
            size += static_cast<std::size_t>(got);
        }
    }
    std::close(file);
    if (!ok) {
        std::free(buffer);
        return 3;
    }
    buffer[size] = '\0';
    *data = buffer;
    *length = size;
    return 0;
}

bool write_all(std::FileHandle file, const char* data, std::size_t length) {
    while (length > 0) {
        const std::uint64_t wrote = std::write(file, data, length);
        if (wrote == kFail || wrote == 0) {
            return false;
        }
        data += wrote;
        length -= static_cast<std::size_t>(wrote);
    }
    return true;
}

class Notepad : public ui::Window {
    // Docked in this order: toolbar, its divider, the status line and its
    // divider, then the text. The sheets come last, over everything.
    ui::Panel toolbar_{this};
    ui::Button new_{&toolbar_};
    ui::Button open_{&toolbar_};
    ui::Button save_{&toolbar_};
    ui::Label name_{&toolbar_};
    ui::TextBox search_{&toolbar_};
    ui::Panel toolbarLine_{this};
    ui::Panel statusBar_{this};
    ui::Label position_{&statusBar_};
    ui::Label info_{&statusBar_};
    ui::Panel statusLine_{this};
    ui::TextEdit editor_{this};
    ui::FileDialog files_{this};
    ui::MessageSheet ask_{this};

    ui::Image newIcon_, openIcon_, saveIcon_, searchIcon_;

    // What waits for the "Save changes?" answer (or for Save as to finish).
    enum class Then : std::uint8_t { Nothing, New, Open, Close };
    enum class Question : std::uint8_t { None, Unsaved, Notice };

    char path_[512] = {};          // "" while untitled
    char note_[96] = {};           // a passing note for the status line ("Saved")
    Then then_ = Then::Nothing;
    Question question_ = Question::None;
    ui::FileDialog::Mode dialogMode_ = ui::FileDialog::Mode::Open;
    int matches_ = 0;

    void on_create() override {
        const ui::Theme& t = ui::theme();
        const ui::Size size = client_size();
        load_icon(newIcon_, "file-plus", kIconSize, t.text);
        load_icon(openIcon_, "folder-open", kIconSize, t.text);
        load_icon(saveIcon_, "save", kIconSize, t.text);
        load_icon(searchIcon_, "search", 14, t.textDisabled);

        toolbar_.set_bounds({ 0, 0, size.width, t.toolbarHeight }).set_dock(ui::Dock::Top);
        const int y = (t.toolbarHeight - kButton) / 2;
        new_.set_icon(&newIcon_).set_flat(true).set_bounds({ 12, y, kButton, kButton });
        open_.set_icon(&openIcon_).set_flat(true).set_bounds({ 12 + kButton + 6, y, kButton, kButton });
        save_.set_icon(&saveIcon_).set_flat(true).set_bounds({ 12 + 2 * (kButton + 6), y, kButton, kButton });
        const int nameX = 12 + 3 * (kButton + 6) + 10;
        name_.set_font(&t.fontMedium())
            .set_bounds({ nameX, y, size.width - nameX - kSearchWidth - 24, kButton })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        search_.set_icon(&searchIcon_).set_placeholder("Find")
            .set_bounds({ size.width - kSearchWidth - 12, y, kSearchWidth, kButton })
            .set_anchor(ui::AnchorTop | ui::AnchorRight);
        toolbarLine_.set_bounds({ 0, 0, size.width, 1 }).set_dock(ui::Dock::Top);
        toolbarLine_.set_background(t.divider);

        statusBar_.set_bounds({ 0, 0, size.width, t.statusHeight - 1 }).set_dock(ui::Dock::Bottom);
        position_.set_font(&t.fontSmall()).set_color(t.textDim)
            .set_bounds({ 18, 0, 200, t.statusHeight - 1 });
        info_.set_font(&t.fontSmall()).set_color(t.textDim).set_align(ui::AlignRight | ui::AlignMiddle)
            .set_bounds({ 220, 0, size.width - 238, t.statusHeight - 1 })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        statusLine_.set_bounds({ 0, 0, size.width, 1 }).set_dock(ui::Dock::Bottom);
        statusLine_.set_background(t.divider);

        editor_.set_dock(ui::Dock::Fill);

        new_.on_click = [this] { request(Then::New); };
        open_.on_click = [this] { request(Then::Open); };
        save_.on_click = [this] { save(); };
        search_.on_change = [this](const char*) { refresh_matches(); };
        search_.on_enter = [this](const char*) { find(true); };
        editor_.on_change = [this] {
            note_[0] = '\0';
            if (search_.text()[0] != '\0') {
                refresh_matches();
            }
            update_name();
            update_status();
        };
        editor_.on_caret = [this] { update_status(); };
        files_.on_accept = [this](const char* path) { chosen(path); };
        files_.on_cancel = [this] { then_ = Then::Nothing; };
        ask_.on_choice = [this](int choice) { answered(choice); };

        if (gStartPath && gStartPath[0] != '\0') {
            open_path(gStartPath, true);
        } else {
            new_document();
        }
        editor_.focus();
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect& dirty) override {
        canvas.fill(dirty, ui::theme().window);
    }

    bool on_key_down(const ui::KeyEvent& key) override {
        if (key.key == std::KeyF3) {
            find(!key.shift());
            return true;
        }
        if (key.key == std::KeyEscape && focused_control() == &search_) {
            editor_.focus();
            return true;
        }
        if (!key.ctrl() || key.alt()) {
            return false;
        }
        switch (key.key) {
        case std::KeyN: request(Then::New); return true;
        case std::KeyO: request(Then::Open); return true;
        case std::KeyS:
            if (key.shift()) {
                save_as();
            } else {
                save();
            }
            return true;
        case std::KeyF:
            search_.focus();
            search_.select_all();
            return true;
        default:
            return false;
        }
    }

    bool on_close() override {
        if (editor_.modified()) {
            request(Then::Close);
            return false;
        }
        return true;
    }

    // ---- documents ---------------------------------------------------------------

    const char* display_name() const {
        return path_[0] ? base_name(path_) : "Untitled";
    }

    // The folder the dialogs start in: the file's, else /home, else /.
    void current_folder(char* out, std::size_t size) const {
        if (path_[0]) {
            copy_text(out, size, path_);
            char* slash = nullptr;
            for (char* c = out; *c; ++c) {
                if (*c == '/') slash = c;
            }
            if (slash == out) {
                slash[1] = '\0';
            } else if (slash) {
                *slash = '\0';
            }
            if (is_folder(out)) {
                return;
            }
        }
        copy_text(out, size, is_folder("/home") ? "/home" : "/");
    }

    void new_document() {
        path_[0] = '\0';
        editor_.set_text("");
        editor_.set_language(ui::Language::Plain);
        note_[0] = '\0';
        refresh_matches();
        update_name();
        update_status();
    }

    // Opens `path`; `orNew`: a file that does not exist starts empty under
    // that name (notepad FILE).
    void open_path(const char* path, bool orNew) {
        char* data = nullptr;
        std::size_t length = 0;
        const int result = read_file(path, &data, &length);
        if (result == 1 && orNew) {
            new_document();
            copy_text(path_, sizeof(path_), path);
            editor_.set_language(ui::language_for(path_));
            copy_text(note_, sizeof(note_), "New file");
            update_name();
            update_status();
            return;
        }
        if (result != 0) {
            char message[160] = {};
            copy_text(message, sizeof(message), base_name(path));
            append_text(message, sizeof(message),
                        result == 2 ? " is larger than 32 MB." : result == 1 ? " is not there." : " could not be read.");
            notice("Could not open the file", message);
            return;
        }
        std::size_t nul = 0;
        for (std::size_t i = 0; i < length && i < 65536; ++i) {
            nul += data[i] == '\0';
        }
        if (nul > 0) {
            std::free(data);
            char message[160] = {};
            copy_text(message, sizeof(message), base_name(path));
            append_text(message, sizeof(message), " is not a text file.");
            notice("Could not open the file", message);
            return;
        }
        if (!editor_.set_text(data, length)) {
            std::free(data);
            notice("Could not open the file", "There is not enough memory for it.");
            return;
        }
        editor_.set_language(ui::language_for(path, data, length));
        std::free(data);
        copy_text(path_, sizeof(path_), path);
        note_[0] = '\0';
        refresh_matches();
        update_name();
        update_status();
    }

    // Writes the text to `path` ("\r\n" lines if it came with them).
    bool write_path(const char* path) {
        const std::FileHandle file = std::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (file == kFail) {
            notice("Could not save the file", "The folder cannot be written to.");
            return false;
        }
        const char* text = editor_.text();
        const std::size_t length = editor_.length();
        bool ok = true;
        if (!editor_.crlf()) {
            ok = write_all(file, text, length);
        } else {
            std::size_t from = 0;
            for (std::size_t i = 0; ok && i <= length; ++i) {
                if (i == length || text[i] == '\n') {
                    ok = write_all(file, text + from, i - from) && (i == length || write_all(file, "\r\n", 2));
                    from = i + 1;
                }
            }
        }
        std::close(file);
        if (!ok) {
            notice("Could not save the file", "Writing it failed; the disk may be full.");
            return false;
        }
        const bool renamed = std::strcmp(path, path_) != 0;
        copy_text(path_, sizeof(path_), path);
        if (renamed) {
            editor_.set_language(ui::language_for(path_, editor_.text(), editor_.length()));
        }
        editor_.set_saved();
        copy_text(note_, sizeof(note_), "Saved");
        update_name();
        update_status();
        return true;
    }

    // Saves to the file it came from; an untitled text asks for a name.
    bool save() {
        if (path_[0] == '\0') {
            save_as();
            return false;
        }
        return write_path(path_);
    }

    void save_as() {
        char folder[512];
        current_folder(folder, sizeof(folder));
        dialogMode_ = ui::FileDialog::Mode::Save;
        files_.show(dialogMode_, folder, path_[0] ? base_name(path_) : "untitled.txt");
    }

    void chosen(const char* path) {
        if (dialogMode_ == ui::FileDialog::Mode::Open) {
            open_path(path, false);
            return;
        }
        if (write_path(path)) {
            perform(then_);   // a Save as asked for by "Save changes?"
        } else {
            then_ = Then::Nothing;
        }
    }

    // ---- the unsaved-changes question ------------------------------------------------

    void request(Then action) {
        if (!editor_.modified()) {
            perform(action);
            return;
        }
        then_ = action;
        question_ = Question::Unsaved;
        char message[160] = {};
        copy_text(message, sizeof(message), display_name());
        append_text(message, sizeof(message), " has changes that are not saved.");
        ask_.show("Save the changes?", message, "Save", "Don't save", "Cancel");
    }

    void answered(int choice) {
        if (question_ != Question::Unsaved) {
            question_ = Question::None;
            return;
        }
        question_ = Question::None;
        if (choice == 0) {
            if (save()) {
                perform(then_);
            }
            // An untitled text: Save as runs then_ once it is written.
        } else if (choice == 1) {
            perform(then_);
        } else {
            then_ = Then::Nothing;
        }
    }

    void perform(Then action) {
        then_ = Then::Nothing;
        switch (action) {
        case Then::New:
            new_document();
            break;
        case Then::Open: {
            char folder[512];
            current_folder(folder, sizeof(folder));
            dialogMode_ = ui::FileDialog::Mode::Open;
            files_.show(dialogMode_, folder);
            break;
        }
        case Then::Close:
            close();
            break;
        case Then::Nothing:
            break;
        }
    }

    void notice(const char* title, const char* message) {
        question_ = Question::Notice;
        ask_.show(title, message, "OK");
    }

    // ---- search ----------------------------------------------------------------

    void refresh_matches() {
        matches_ = editor_.set_find_highlight(search_.text());
        update_status();
    }

    void find(bool forward) {
        if (search_.text()[0] == '\0') {
            search_.focus();
            return;
        }
        if (!editor_.find(search_.text(), forward)) {
            copy_text(note_, sizeof(note_), "Not found");
            update_status();
        }
    }

    // ---- what the window shows -----------------------------------------------------

    void update_name() {
        char line[160] = {};
        copy_text(line, sizeof(line), display_name());
        if (editor_.modified()) {
            append_text(line, sizeof(line), "  (edited)");
        }
        name_.set_text(line);
    }

    void update_status() {
        char left[64] = "Ln ";
        append_int(left, sizeof(left), static_cast<std::uint64_t>(editor_.caret_line() + 1));
        append_text(left, sizeof(left), ", Col ");
        append_int(left, sizeof(left), static_cast<std::uint64_t>(editor_.caret_column() + 1));
        if (editor_.has_selection()) {
            append_text(left, sizeof(left), "  (");
            append_int(left, sizeof(left), editor_.selection_end() - editor_.selection_start());
            append_text(left, sizeof(left), " selected)");
        }
        position_.set_text(left);

        char right[192] = {};
        if (note_[0]) {
            append_text(right, sizeof(right), note_);
            append_text(right, sizeof(right), "  -  ");
        }
        if (search_.text()[0] != '\0') {
            append_int(right, sizeof(right), static_cast<std::uint64_t>(matches_));
            append_text(right, sizeof(right), matches_ == 1 ? " match  -  " : " matches  -  ");
        }
        append_int(right, sizeof(right), static_cast<std::uint64_t>(editor_.line_count()));
        append_text(right, sizeof(right), editor_.line_count() == 1 ? " line  -  " : " lines  -  ");
        append_text(right, sizeof(right), ui::language_name(editor_.language()));
        append_text(right, sizeof(right), editor_.use_tabs() ? "  -  Tabs" : "  -  Spaces: 4");
        append_text(right, sizeof(right), editor_.crlf() ? "  -  CRLF" : "  -  LF");
        info_.set_text(right);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        gStartPath = argv[1];
    }
    return ui::run<Notepad>({ .width = 860, .height = 600, .title = "Notepad" });
}
