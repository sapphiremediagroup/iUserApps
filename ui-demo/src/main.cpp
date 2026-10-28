#include <cstring.hpp>
#include <ui/ui.hpp>

constexpr int kMaxItems = 200;
constexpr int kInitialItems = 100;

class Demo : public ui::Window {
    ui::Panel toolbar_{this};
    ui::Label nameLabel_{&toolbar_, "Name"};
    ui::TextBox name_{&toolbar_};
    ui::Button add_{&toolbar_, "Add"};
    ui::ListBox list_{this};
    ui::Panel side_{this};
    ui::ImageView image_{&side_};
    ui::CheckBox showImage_{&side_, "Show the logo"};
    ui::Label selection_{&side_, "Nothing selected"};
    ui::Label status_{this, "Tab moves between controls; Enter is OK"};
    ui::Button ok_{this, "OK"};
    ui::Button clear_{this, "Clear"};

    ui::Image logo_;
    char items_[kMaxItems][32] = {};
    int itemCount_ = 0;
    char line_[96] = {};

    void set_status(const char* what, const char* name) {
        std::strncpy(line_, what, sizeof(line_) - 1);
        const std::size_t used = std::strlen(line_);
        std::strncpy(line_ + used, name, sizeof(line_) - used - 1);
        status_.set_text(line_);
    }

    void add_item() {
        if (name_.text()[0] == '\0' || itemCount_ == kMaxItems) {
            return;
        }
        std::strncpy(items_[itemCount_], name_.text(), sizeof(items_[0]) - 1);
        ++itemCount_;
        list_.set_count(itemCount_);
        list_.set_selected(itemCount_ - 1);
        set_status("Added ", name_.text());
        name_.set_text("");
    }

    void on_create() override {
        const ui::Theme& t = ui::theme();
        const ui::Size size = client_size();
        const int pad = t.spacing;
        const int h = t.controlHeight;

        toolbar_.set_bounds({ 0, 0, size.width, h + 2 * pad }).set_dock(ui::Dock::Top);
        toolbar_.set_background(t.face);
        nameLabel_.set_bounds({ pad, pad, 48, h });
        name_.set_bounds({ pad + 52, pad, size.width - 52 - 80 - 3 * pad, h })
            .set_anchor(ui::AnchorLeft | ui::AnchorTop | ui::AnchorRight);
        name_.set_placeholder("Type a name, then Enter or Add");
        add_.set_bounds({ size.width - 80 - pad, pad, 80, h }).set_anchor(ui::AnchorTop | ui::AnchorRight);

        const int top = h + 3 * pad;
        const int bottom = h + 2 * pad;
        const int sideWidth = 220;
        list_.set_bounds({ pad, top, size.width - sideWidth - 3 * pad, size.height - top - bottom })
            .set_anchor(ui::AnchorAll);
        side_.set_bounds({ size.width - sideWidth - pad, top, sideWidth, size.height - top - bottom })
            .set_anchor(ui::AnchorTop | ui::AnchorRight | ui::AnchorBottom);
        side_.set_border(true);
        image_.set_bounds({ pad, pad, sideWidth - 2 * pad, 90 });
        showImage_.set_bounds({ pad, 90 + 2 * pad, sideWidth - 2 * pad, h });
        selection_.set_bounds({ pad, 90 + 3 * pad + h, sideWidth - 2 * pad, h });

        status_.set_bounds({ pad, size.height - h - pad, size.width - 2 * 90 - 4 * pad, h })
            .set_anchor(ui::AnchorLeft | ui::AnchorRight | ui::AnchorBottom);
        status_.set_color(t.textDim);
        clear_.set_bounds({ size.width - 2 * 90 - 2 * pad, size.height - h - pad, 90, h })
            .set_anchor(ui::AnchorRight | ui::AnchorBottom);
        ok_.set_bounds({ size.width - 90 - pad, size.height - h - pad, 90, h })
            .set_anchor(ui::AnchorRight | ui::AnchorBottom);
        set_default_button(&ok_);
        set_cancel_button(&clear_);

        for (itemCount_ = 0; itemCount_ < kInitialItems; ++itemCount_) {
            char* item = items_[itemCount_];
            std::strcpy(item, "Item ");
            int n = itemCount_ + 1;
            char digits[8];
            int count = 0;
            while (n > 0) {
                digits[count++] = static_cast<char>('0' + n % 10);
                n /= 10;
            }
            int at = 5;
            while (count > 0) {
                item[at++] = digits[--count];
            }
            item[at] = '\0';
        }
        list_.row_text = [this](int row) -> const char* { return items_[row]; };
        list_.set_count(itemCount_);
        list_.on_select = [this](int row) {
            selection_.set_text(row >= 0 ? items_[row] : "Nothing selected");
        };
        list_.on_activate = [this](int row) { set_status("Opened ", items_[row]); };

        logo_.load("/bin/images/InstantOS_sapph.png");
        image_.set_image(&logo_);
        showImage_.set_checked(true);
        showImage_.on_change = [this](bool on) { image_.set_visible(on); };

        name_.on_enter = [this](const char*) { add_item(); };
        add_.on_click = [this] { add_item(); };
        clear_.on_click = [this] {
            name_.set_text("");
            set_status("Cleared", "");
        };
        ok_.on_click = [this] { close(); };
        name_.focus();
    }
};

int main(int, char**) {
    return ui::run<Demo>({ .width = 640, .height = 440, .title = "UI demo" });
}
