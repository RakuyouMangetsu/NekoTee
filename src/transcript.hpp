#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vterm.h>

namespace neko {

class Transcript {
public:
    Transcript(int rows, int columns);

    std::string feed(std::string_view bytes);
    std::string resize(int rows, int columns);

private:
    struct Row {
        std::string text;
        bool dirty = false;
    };

    static int damage(VTermRect rect, void* self);
    static int move(VTermRect dest, VTermRect src, void* self);
    static int property(VTermProp prop, VTermValue* value, void* self);
    static int scrollback(int columns, const VTermScreenCell* cells, void* self);

    std::vector<Row>& active() { return alternate_active_ ? alternate_ : primary_; }
    void publish();
    void publish_row(int row, std::string text, bool complete);
    void close_line();
    void input(std::string_view bytes);

    std::unique_ptr<VTerm, decltype(&vterm_free)> vterm_;
    VTermScreen* screen_;
    VTermState* state_;
    std::vector<Row> primary_;
    std::vector<Row> alternate_;
    std::string output_;
    int columns_;
    int open_row_ = -1;
    int scroll_index_ = 0;
    unsigned int scroll_events_ = 0;
    bool alternate_active_ = false;
    bool resizing_ = false;
};

} // namespace neko
