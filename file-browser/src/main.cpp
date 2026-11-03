#include <cstdint.hpp>
#include <cstring.hpp>
#include <syscall.hpp>
#include <ui/ui.hpp>

namespace {
constexpr std::uint64_t fail = static_cast<std::uint64_t>(-1);
constexpr std::uint32_t kMaxEntries = 512;
constexpr int kMaxPlaces = 16;
constexpr int kHistory = 32;
constexpr int kIconSize = 16;
constexpr int kNavButton = 28;
constexpr int kSearchWidth = 200;
constexpr int kKindColumn = 260;    // from the list's right edge
constexpr int kSizeColumn = 110;

struct BrowserEntry {
    std::DirEntry dir;
    bool executable;
    std::uint64_t size;
};

BrowserEntry gEntries[kMaxEntries];
std::DirEntry gDirScratch[kMaxEntries];

void append_text(char* buffer, std::size_t capacity, const char* text) {
    std::size_t len = std::strlen(buffer);
    std::size_t index = 0;
    while (text[index] != '\0' && len + 1 < capacity) {
        buffer[len++] = text[index++];
    }
    buffer[len] = '\0';
}

void append_uint(char* buffer, std::size_t capacity, std::uint64_t value) {
    char digits[24];
    std::size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value > 0);
    char text[24];
    for (std::size_t i = 0; i < count; ++i) {
        text[i] = digits[count - 1 - i];
    }
    text[count] = '\0';
    append_text(buffer, capacity, text);
}

void copy_string(char* buffer, std::size_t capacity, const char* text) {
    std::strncpy(buffer, text, capacity - 1);
    buffer[capacity - 1] = '\0';
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Whether `name` contains `needle`, ignoring case.
bool contains_folded(const char* name, const char* needle) {
    if (needle[0] == '\0') {
        return true;
    }
    for (std::size_t i = 0; name[i] != '\0'; ++i) {
        std::size_t j = 0;
        while (needle[j] != '\0' && ascii_lower(name[i + j]) == ascii_lower(needle[j])) {
            ++j;
        }
        if (needle[j] == '\0') {
            return true;
        }
    }
    return false;
}

const char* extension_of(const char* name) {
    const char* dot = nullptr;
    for (const char* c = name; *c != '\0'; ++c) {
        if (*c == '.') {
            dot = c;
        }
    }
    return dot ? dot + 1 : "";
}

bool equals_folded(const char* a, const char* b) {
    for (; *a != '\0' && *b != '\0'; ++a, ++b) {
        if (ascii_lower(*a) != ascii_lower(*b)) {
            return false;
        }
    }
    return *a == *b;
}

// Files without a dot in the name are programs (/bin has no extensions).
bool is_launchable_name(const char* name) {
    for (std::size_t i = 0; name[i] != '\0'; ++i) {
        if (name[i] == '.') {
            return false;
        }
    }
    return name[0] != '\0';
}

const char* kind_of(const BrowserEntry& entry) {
    if (entry.dir.type == std::FileType::Directory) {
        return "Folder";
    }
    if (entry.executable) {
        return "Program";
    }
    const char* ext = extension_of(entry.dir.name);
    if (equals_folded(ext, "ttf") || equals_folded(ext, "otf")) {
        return "Font";
    }
    if (equals_folded(ext, "png") || equals_folded(ext, "jpg") || equals_folded(ext, "svg")) {
        return "Image";
    }
    if (equals_folded(ext, "txt") || equals_folded(ext, "md") || equals_folded(ext, "c") ||
        equals_folded(ext, "h") || equals_folded(ext, "map")) {
        return "Text";
    }
    return "File";
}

// "96 KB", "1.3 MB"; "-" for folders.
void format_size(const BrowserEntry& entry, char* out, std::size_t size) {
    out[0] = '\0';
    if (entry.dir.type == std::FileType::Directory) {
        append_text(out, size, "-");
        return;
    }
    const std::uint64_t bytes = entry.size;
    if (bytes < 1024) {
        append_uint(out, size, bytes);
        append_text(out, size, " B");
    } else if (bytes < 1024 * 1024) {
        append_uint(out, size, (bytes + 512) / 1024);
        append_text(out, size, " KB");
    } else {
        const std::uint64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
        append_uint(out, size, tenths / 10);
        append_text(out, size, ".");
        append_uint(out, size, tenths % 10);
        append_text(out, size, " MB");
    }
}

bool path_is_root(const char* path) {
    return path[0] == '/' && path[1] == '\0';
}

void build_child_path(const char* base, const char* name, char* output, std::size_t outputSize) {
    output[0] = '\0';
    append_text(output, outputSize, base);
    if (!path_is_root(base)) {
        append_text(output, outputSize, "/");
    }
    append_text(output, outputSize, name);
}

void build_parent_path(const char* current, char* output, std::size_t outputSize) {
    copy_string(output, outputSize, current);
    std::size_t length = std::strlen(output);
    while (length > 1 && output[length - 1] == '/') {
        output[--length] = '\0';
    }
    while (length > 1 && output[length - 1] != '/') {
        output[--length] = '\0';
    }
    if (length > 1 && output[length - 1] == '/') {
        output[length - 1] = '\0';
    }
    if (output[0] == '\0') {
        copy_string(output, outputSize, "/");
    }
}

int compare_names(const char* lhs, const char* rhs) {
    for (std::size_t index = 0;; ++index) {
        const char a = ascii_lower(lhs[index]);
        const char b = ascii_lower(rhs[index]);
        if (a != b) {
            return a < b ? -1 : 1;
        }
        if (a == '\0') {
            return 0;
        }
    }
}

// Folders first, then by name ignoring case.
bool entry_before(const BrowserEntry& lhs, const BrowserEntry& rhs) {
    const bool lhsDir = lhs.dir.type == std::FileType::Directory;
    const bool rhsDir = rhs.dir.type == std::FileType::Directory;
    if (lhsDir != rhsDir) {
        return lhsDir;
    }
    return compare_names(lhs.dir.name, rhs.dir.name) < 0;
}

void sort_entries(BrowserEntry* entries, std::uint32_t count) {
    for (std::uint32_t i = 1; i < count; ++i) {
        const BrowserEntry moving = entries[i];
        std::uint32_t j = i;
        while (j > 0 && entry_before(moving, entries[j - 1])) {
            entries[j] = entries[j - 1];
            --j;
        }
        entries[j] = moving;
    }
}

// A /bin/icons line icon at `size`, in `color`.
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

// The current folder as clickable segments, "/ > bin > icons", the last
// one as a chip.
class Breadcrumb : public ui::Control {
public:
    using Control::Control;
    ui::Callback<void(const char*)> on_navigate;
    const ui::Image* chevron = nullptr;

    void set_path(const char* path) {
        copy_string(path_, sizeof(path_), path);
        invalidate();
    }

protected:
    void paint(ui::Canvas& canvas) override {
        const ui::Theme& t = ui::theme();
        const ui::Rect all = { 0, 0, bounds().width, bounds().height };
        canvas.fill_rounded(all, t.fieldRadius, t.field);
        canvas.frame(all, t.divider, 1, t.fieldRadius);
        Segment segments[kMaxSegments];
        const int count = layout(segments);
        for (int i = 0; i < count; ++i) {
            const Segment& s = segments[i];
            const bool last = i == count - 1;
            if (last && i > 0) {
                canvas.fill_rounded({ s.rect.x - 6, 4, s.rect.width + 12, all.height - 8 }, 5, t.face);
            }
            if (i > 0 && chevron && chevron->valid()) {
                canvas.image(*chevron, ui::Point { s.rect.x - 8 - chevron->width() - 2,
                                                   (all.height - chevron->height()) / 2 });
            }
            canvas.text(s.rect, s.name, last && i > 0 ? t.fontMedium() : t.font(), last ? t.text : t.textDim,
                        ui::AlignLeft | ui::AlignMiddle);
        }
    }

    void mouse_down(const ui::MouseEvent& mouse) override {
        if (mouse.button != ui::MouseLeft) {
            return;
        }
        Segment segments[kMaxSegments];
        const int count = layout(segments);
        for (int i = 0; i < count - 1; ++i) {
            if (segments[i].rect.contains(mouse.x, mouse.y) && on_navigate) {
                on_navigate(segments[i].path);
                return;
            }
        }
    }

private:
    static constexpr int kMaxSegments = 12;
    struct Segment {
        char name[64];
        char path[256];
        ui::Rect rect;
    };

    int layout(Segment* segments) const {
        const ui::Theme& t = ui::theme();
        const int chevronWidth = chevron && chevron->valid() ? chevron->width() : 0;
        int count = 0;
        int x = 12;
        auto add = [&](const char* name, const char* path, bool chip) {
            if (count == kMaxSegments) {
                return;
            }
            Segment& s = segments[count++];
            copy_string(s.name, sizeof(s.name), name);
            copy_string(s.path, sizeof(s.path), path);
            const int width = (chip ? t.fontMedium() : t.font()).width(s.name);
            s.rect = { x, 0, width, bounds().height };
            x += width + 8 + chevronWidth + 10;
        };
        char path[256] = "/";
        add("/", "/", false);
        const char* p = path_;
        while (*p != '\0') {
            while (*p == '/') {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            char name[64] = {};
            std::size_t n = 0;
            while (p[n] != '\0' && p[n] != '/' && n + 1 < sizeof(name)) {
                name[n] = p[n];
                ++n;
            }
            p += n;
            if (!path_is_root(path)) {
                append_text(path, sizeof(path), "/");
            }
            append_text(path, sizeof(path), name);
            add(name, path, *p == '\0');
        }
        return count;
    }

    char path_[256] = "/";
};

// The list's column titles, lined up with the rows.
class ColumnHeader : public ui::Control {
public:
    using Control::Control;

protected:
    void paint(ui::Canvas& canvas) override {
        const ui::Theme& t = ui::theme();
        const int w = bounds().width - t.scrollBarWidth;
        const int h = bounds().height;
        canvas.text({ 44, 0, w - kKindColumn - 44, h }, "Name", t.fontSmall(), t.textDim, ui::AlignLeft | ui::AlignMiddle);
        canvas.text({ w - kKindColumn, 0, 120, h }, "Kind", t.fontSmall(), t.textDim, ui::AlignLeft | ui::AlignMiddle);
        canvas.text({ w - kSizeColumn, 0, 100, h }, "Size", t.fontSmall(), t.textDim, ui::AlignLeft | ui::AlignMiddle);
        canvas.fill({ 0, h - 1, bounds().width, 1 }, t.divider);
    }
};

class FileBrowser : public ui::Window {
    // Docked in this order: toolbar, its divider, the status line and its
    // divider, the sidebar and its divider, then the list area.
    ui::Panel toolbar_{this};
    ui::Button back_{&toolbar_};
    ui::Button forward_{&toolbar_};
    ui::Button up_{&toolbar_};
    Breadcrumb crumbs_{&toolbar_};
    ui::TextBox search_{&toolbar_};
    ui::Panel toolbarLine_{this};
    ui::Panel statusBar_{this};
    ui::Label status_{&statusBar_};
    ui::Panel statusLine_{this};
    ui::Panel sidebar_{this};
    ui::Label placesLabel_{&sidebar_, "PLACES"};
    ui::ListBox places_{&sidebar_};
    ui::Panel sidebarLine_{this};
    ui::Panel content_{this};
    ColumnHeader header_{&content_};
    ui::ListBox list_{&content_};

    char currentPath_[256] = "/bin";
    std::uint32_t count_ = 0;
    std::uint32_t visible_[kMaxEntries] = {};
    std::uint32_t visibleCount_ = 0;
    char placeNames_[kMaxPlaces][64] = {};
    int placeCount_ = 0;
    char backStack_[kHistory][256] = {};
    int backCount_ = 0;
    char forwardStack_[kHistory][256] = {};
    int forwardCount_ = 0;
    char message_[160] = {};
    bool messageIsError_ = false;

    ui::Image folderIcon_, programIcon_, fileIcon_;
    ui::Image placeIcon_, placeIconSelected_;
    ui::Image backIcon_, backIconOff_, forwardIcon_, forwardIconOff_, upIcon_, upIconOff_;
    ui::Image searchIcon_, chevronIcon_;

    void on_create() override {
        const ui::Theme& t = ui::theme();
        const ui::Size size = client_size();
        load_icons();

        toolbar_.set_bounds({ 0, 0, size.width, t.toolbarHeight }).set_dock(ui::Dock::Top);
        const int navY = (t.toolbarHeight - kNavButton) / 2;
        back_.set_icon(&backIcon_, &backIconOff_).set_flat(true).set_bounds({ 12, navY, kNavButton, kNavButton });
        forward_.set_icon(&forwardIcon_, &forwardIconOff_).set_flat(true)
            .set_bounds({ 12 + kNavButton + 6, navY, kNavButton, kNavButton });
        up_.set_icon(&upIcon_, &upIconOff_).set_flat(true)
            .set_bounds({ 12 + 2 * (kNavButton + 6), navY, kNavButton, kNavButton });
        const int crumbsX = 12 + 3 * (kNavButton + 6) + 6;
        crumbs_.chevron = &chevronIcon_;
        crumbs_.set_bounds({ crumbsX, navY, size.width - crumbsX - kSearchWidth - 24, kNavButton })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        search_.set_icon(&searchIcon_).set_placeholder("Search")
            .set_bounds({ size.width - kSearchWidth - 12, navY, kSearchWidth, kNavButton })
            .set_anchor(ui::AnchorTop | ui::AnchorRight);
        toolbarLine_.set_bounds({ 0, 0, size.width, 1 }).set_dock(ui::Dock::Top);
        toolbarLine_.set_background(t.divider);

        statusBar_.set_bounds({ 0, 0, size.width, t.statusHeight - 1 }).set_dock(ui::Dock::Bottom);
        status_.set_font(&t.fontSmall()).set_color(t.textDim)
            .set_bounds({ 18, 0, size.width - 36, t.statusHeight - 1 })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        statusLine_.set_bounds({ 0, 0, size.width, 1 }).set_dock(ui::Dock::Bottom);
        statusLine_.set_background(t.divider);

        const int bodyHeight = size.height - t.toolbarHeight - t.statusHeight - 1;
        sidebar_.set_bounds({ 0, 0, t.sidebarWidth, bodyHeight }).set_dock(ui::Dock::Left);
        sidebar_.set_background(t.panel);
        placesLabel_.set_font(&t.fontLabel()).set_color(t.textDisabled).set_bounds({ 18, 10, t.sidebarWidth - 36, 20 });
        const int placesTop = 34;
        places_.set_sidebar(true).set_bounds({ 0, placesTop, t.sidebarWidth, bodyHeight - placesTop })
            .set_anchor(ui::AnchorAll);
        sidebarLine_.set_bounds({ 0, 0, 1, bodyHeight }).set_dock(ui::Dock::Left);
        sidebarLine_.set_background(t.divider);

        content_.set_dock(ui::Dock::Fill);
        header_.set_bounds({ 0, 0, size.width, 31 }).set_dock(ui::Dock::Top);
        list_.set_dock(ui::Dock::Fill);

        back_.on_click = [this] { go_back(); };
        forward_.on_click = [this] { go_forward(); };
        up_.on_click = [this] { open_parent(); };
        crumbs_.on_navigate = [this](const char* path) { navigate(path); };
        search_.on_change = [this](const char*) { apply_filter(nullptr); };
        search_.on_enter = [this](const char*) { list_.focus(); };
        list_.row_text = [this](int row) -> const char* { return gEntries[visible_[row]].dir.name; };
        list_.draw_row = [this](ui::Canvas& canvas, int row, const ui::Rect& rect, bool selected) {
            draw_entry(canvas, row, rect, selected);
        };
        list_.on_activate = [this](int row) { open_entry(row); };
        list_.on_select = [this](int) { show_status(); };
        places_.row_text = [this](int row) -> const char* { return placeNames_[row]; };
        places_.draw_row = [this](ui::Canvas& canvas, int row, const ui::Rect& rect, bool selected) {
            draw_place(canvas, row, rect, selected);
        };
        places_.on_activate = [this](int row) { open_place(row); };
        places_.on_select = [this](int row) {
            if (places_.focused()) {
                open_place(row);
            }
        };

        load_places();
        load_directory();
        list_.focus();
    }

    void on_paint(ui::Canvas& canvas, const ui::Rect& dirty) override {
        canvas.fill(dirty, ui::theme().window);
    }

    bool on_key_down(const ui::KeyEvent& key) override {
        if (key.ctrl() && key.key == std::KeyF) {
            search_.focus();
            search_.select_all();
            return true;
        }
        if (key.ctrl()) {
            return false;
        }
        if (key.alt()) {
            if (key.key == std::KeyUp) {
                open_parent();
                return true;
            }
            if (key.key == std::KeyLeft) {
                go_back();
                return true;
            }
            if (key.key == std::KeyRight) {
                go_forward();
                return true;
            }
            return false;
        }
        switch (key.key) {
        case std::KeyBackspace:
            open_parent();
            return true;
        case std::KeyF5:
            load_directory(selected_name());
            return true;
        case std::KeyEscape:
            if (search_.text()[0] != '\0') {
                search_.set_text("");
                apply_filter(nullptr);
                list_.focus();
            } else {
                close();
            }
            return true;
        default:
            return false;
        }
    }

    void load_icons() {
        const ui::Theme& t = ui::theme();
        load_icon(folderIcon_, "folder", kIconSize, t.kindFolder);
        load_icon(programIcon_, "app", kIconSize, t.kindProgram);
        load_icon(fileIcon_, "file", kIconSize, t.kindFile);
        load_icon(placeIcon_, "folder", 15, t.textDim);
        load_icon(placeIconSelected_, "folder", 15, t.accentLight);
        load_icon(backIcon_, "back", kIconSize, t.text);
        load_icon(backIconOff_, "back", kIconSize, t.textDisabled);
        load_icon(forwardIcon_, "forward", kIconSize, t.text);
        load_icon(forwardIconOff_, "forward", kIconSize, t.textDisabled);
        load_icon(upIcon_, "up", kIconSize, t.text);
        load_icon(upIconOff_, "up", kIconSize, t.textDisabled);
        load_icon(searchIcon_, "search", 14, t.textDisabled);
        load_icon(chevronIcon_, "chevron", 12, t.textDisabled);
    }

    void draw_entry(ui::Canvas& canvas, int row, const ui::Rect& rect, bool selected) {
        const ui::Theme& t = ui::theme();
        const BrowserEntry& entry = gEntries[visible_[row]];
        const ui::Image& icon = entry.dir.type == std::FileType::Directory ? folderIcon_
                              : entry.executable ? programIcon_ : fileIcon_;
        if (icon.valid()) {
            canvas.image(icon, ui::Point { rect.x + 20, rect.y + (rect.height - kIconSize) / 2 });
        }
        // Columns from the list's width less the scroll bar, as the header
        // has them, whether the bar shows or not.
        const int right = list_.bounds().width - t.scrollBarWidth;
        const bool strong = selected && list_.focused();
        canvas.text({ 44, rect.y, right - kKindColumn - 52, rect.height }, entry.dir.name,
                    selected ? t.fontMedium() : t.font(), strong ? t.selectionText : t.text,
                    ui::AlignLeft | ui::AlignMiddle);
        canvas.text({ right - kKindColumn, rect.y, 120, rect.height }, kind_of(entry), t.font(), t.textDim,
                    ui::AlignLeft | ui::AlignMiddle);
        char size[24];
        format_size(entry, size, sizeof(size));
        canvas.text({ right - kSizeColumn, rect.y, 100, rect.height }, size, t.font(), t.textDim,
                    ui::AlignLeft | ui::AlignMiddle);
    }

    void draw_place(ui::Canvas& canvas, int row, const ui::Rect& rect, bool selected) {
        const ui::Theme& t = ui::theme();
        const ui::Image& icon = selected ? placeIconSelected_ : placeIcon_;
        if (icon.valid()) {
            canvas.image(icon, ui::Point { rect.x + 20, rect.y + (rect.height - icon.height()) / 2 });
        }
        canvas.text({ rect.x + 44, rect.y, rect.width - 52, rect.height }, placeNames_[row],
                    selected ? t.fontMedium() : t.font(), selected ? t.text : t.textDim, ui::AlignLeft | ui::AlignMiddle);
    }

    // ---- places ----------------------------------------------------------------

    void load_places() {
        placeCount_ = 0;
        copy_string(placeNames_[placeCount_++], sizeof(placeNames_[0]), "Root");
        const std::uint64_t found = std::readdir("/", gDirScratch, kMaxEntries);
        for (std::uint64_t i = 0; found != fail && i < found && placeCount_ < kMaxPlaces; ++i) {
            const std::DirEntry& dir = gDirScratch[i];
            if (dir.type != std::FileType::Directory || dir.name[0] == '.') {
                continue;
            }
            copy_string(placeNames_[placeCount_++], sizeof(placeNames_[0]), dir.name);
        }
        // By name, Root first.
        for (int i = 2; i < placeCount_; ++i) {
            for (int j = i; j > 1 && compare_names(placeNames_[j], placeNames_[j - 1]) < 0; --j) {
                char temp[64];
                copy_string(temp, sizeof(temp), placeNames_[j]);
                copy_string(placeNames_[j], sizeof(temp), placeNames_[j - 1]);
                copy_string(placeNames_[j - 1], sizeof(temp), temp);
            }
        }
        places_.set_count(placeCount_);
    }

    // The place the current folder is in: its first component, or Root.
    void sync_place() {
        const char* p = currentPath_ + 1;
        int match = 0;
        for (int i = 1; i < placeCount_; ++i) {
            const std::size_t n = std::strlen(placeNames_[i]);
            if (std::strncmp(p, placeNames_[i], n) == 0 && (p[n] == '\0' || p[n] == '/')) {
                match = i;
                break;
            }
        }
        places_.set_selected(match);
    }

    void open_place(int row) {
        if (row <= 0) {
            navigate("/");
            return;
        }
        char path[256] = "/";
        append_text(path, sizeof(path), placeNames_[row]);
        navigate(path);
    }

    // ---- folders ---------------------------------------------------------------

    const char* selected_name() const {
        const int row = list_.selected();
        return row >= 0 && static_cast<std::uint32_t>(row) < visibleCount_ ? gEntries[visible_[row]].dir.name : nullptr;
    }

    static void push(char stack[][256], int& count, const char* path) {
        if (count == kHistory) {
            for (int i = 1; i < kHistory; ++i) {
                copy_string(stack[i - 1], 256, stack[i]);
            }
            --count;
        }
        copy_string(stack[count++], 256, path);
    }

    // Opens `path`, remembering where we were for Back.
    void navigate(const char* path, const char* select = nullptr) {
        if (std::strcmp(path, currentPath_) == 0) {
            return;
        }
        push(backStack_, backCount_, currentPath_);
        forwardCount_ = 0;
        copy_string(currentPath_, sizeof(currentPath_), path);
        search_.set_text("");
        load_directory(select);
    }

    void go_back() {
        if (backCount_ == 0) {
            return;
        }
        push(forwardStack_, forwardCount_, currentPath_);
        copy_string(currentPath_, sizeof(currentPath_), backStack_[--backCount_]);
        search_.set_text("");
        load_directory();
        list_.focus();
    }

    void go_forward() {
        if (forwardCount_ == 0) {
            return;
        }
        push(backStack_, backCount_, currentPath_);
        copy_string(currentPath_, sizeof(currentPath_), forwardStack_[--forwardCount_]);
        search_.set_text("");
        load_directory();
        list_.focus();
    }

    void open_parent() {
        if (path_is_root(currentPath_)) {
            return;
        }
        const char* name = currentPath_;
        for (const char* c = currentPath_; *c != '\0'; ++c) {
            if (*c == '/') {
                name = c + 1;
            }
        }
        char left[256];
        copy_string(left, sizeof(left), name);
        char parent[256];
        build_parent_path(currentPath_, parent, sizeof(parent));
        navigate(parent, left);
        list_.focus();   // a toolbar button does not keep the focus
    }

    // Rereads currentPath_, selecting `select` when it is there.
    void load_directory(const char* select = nullptr) {
        char keep[256] = {};
        if (select) {
            copy_string(keep, sizeof(keep), select);
        }
        crumbs_.set_path(currentPath_);
        message_[0] = '\0';
        count_ = 0;
        const std::uint64_t found = std::readdir(currentPath_, gDirScratch, kMaxEntries);
        for (std::uint64_t index = 0; found != fail && index < found && count_ < kMaxEntries; ++index) {
            const std::DirEntry& dir = gDirScratch[index];
            if (std::strcmp(dir.name, ".") == 0 || std::strcmp(dir.name, "..") == 0) {
                continue;
            }
            BrowserEntry& entry = gEntries[count_++];
            entry.dir = dir;
            entry.executable = dir.type == std::FileType::Regular && is_launchable_name(dir.name);
            entry.size = 0;
            if (dir.type == std::FileType::Regular) {
                char path[256];
                build_child_path(currentPath_, dir.name, path, sizeof(path));
                std::Stat stat = {};
                if (std::stat(path, &stat) != fail) {
                    entry.size = static_cast<std::uint64_t>(stat.st_size);
                }
            }
        }
        sort_entries(gEntries, count_);
        apply_filter(keep);
        if (found == fail) {
            set_message("Cannot read this folder", true);
        }
        back_.set_enabled(backCount_ > 0);
        forward_.set_enabled(forwardCount_ > 0);
        up_.set_enabled(!path_is_root(currentPath_));
        sync_place();
    }

    // Shows the entries whose name contains the search text, selecting
    // `select`, or (null) what is selected now, when it is shown.
    void apply_filter(const char* select) {
        char keep[256] = {};
        const char* previous = select ? select : selected_name();
        if (previous) {
            copy_string(keep, sizeof(keep), previous);
        }
        visibleCount_ = 0;
        int selected = -1;
        for (std::uint32_t i = 0; i < count_; ++i) {
            if (!contains_folded(gEntries[i].dir.name, search_.text())) {
                continue;
            }
            if (keep[0] != '\0' && selected < 0 && std::strcmp(gEntries[i].dir.name, keep) == 0) {
                selected = static_cast<int>(visibleCount_);
            }
            visible_[visibleCount_++] = i;
        }
        if (selected < 0 && visibleCount_ > 0) {
            selected = 0;
        }
        list_.set_count(static_cast<int>(visibleCount_));
        list_.set_selected(selected);
        list_.ensure_visible(selected);
        list_.invalidate();
        show_status();
    }

    void set_message(const char* text, bool error) {
        copy_string(message_, sizeof(message_), text);
        messageIsError_ = error;
        show_status();
    }

    // "36 items  ·  backgrounds selected", or the last message.
    void show_status() {
        const ui::Theme& t = ui::theme();
        if (message_[0] != '\0') {
            status_.set_text(message_).set_color(messageIsError_ ? t.danger : t.textDim);
            message_[0] = '\0';   // until the next change
            return;
        }
        char line[200] = {};
        if (count_ == 0) {
            append_text(line, sizeof(line), "Empty folder");
        } else {
            if (visibleCount_ != count_) {
                append_uint(line, sizeof(line), visibleCount_);
                append_text(line, sizeof(line), " of ");
            }
            append_uint(line, sizeof(line), count_);
            append_text(line, sizeof(line), count_ == 1 ? " item" : " items");
            if (const char* name = selected_name()) {
                append_text(line, sizeof(line), "  -  ");
                append_text(line, sizeof(line), name);
                append_text(line, sizeof(line), " selected");
            }
        }
        status_.set_text(line).set_color(t.textDim);
    }

    void open_entry(int row) {
        if (row < 0 || static_cast<std::uint32_t>(row) >= visibleCount_) {
            return;
        }
        const BrowserEntry& entry = gEntries[visible_[row]];
        char target[256];
        build_child_path(currentPath_, entry.dir.name, target, sizeof(target));

        if (entry.dir.type == std::FileType::Directory) {
            navigate(target);
            return;
        }
        if (!entry.executable) {
            set_message("Not a program", true);
            return;
        }
        const std::uint64_t pid = std::spawn(target);
        if (pid == fail) {
            set_message("Could not start it", true);
            return;
        }
        char line[160] = {};
        append_text(line, sizeof(line), "Started ");
        append_text(line, sizeof(line), entry.dir.name);
        set_message(line, false);
    }
};
}

int main(int, char**) {
    return ui::run<FileBrowser>({ .width = 820, .height = 540, .title = "File Browser" });
}
