#pragma once

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <vterm.h>

namespace neko {

class Transcript {
public:
    Transcript(int rows, int columns);

    std::string feed(std::string_view bytes);
    void resize(int rows, int columns);

private:
    static int damage(VTermRect rect, void* user);
    static int move(VTermRect dest, VTermRect src, void* user);
    static int property(VTermProp prop, VTermValue* value, void* user);
    static int scrollback(int columns, const VTermScreenCell* cells, void* user);

    void append_line(std::string& output, int last_row) const;
    void clear_wrapped_prefix();
    void input(std::string_view bytes);

    std::unique_ptr<VTerm, decltype(&vterm_free)> vterm_;
    VTermScreen* screen_;
    VTermState* state_;
    std::deque<std::string> wrapped_prefix_;
    std::size_t wrapped_prefix_bytes_ = 0;
    std::optional<std::string> scrolled_row_;
    int columns_;
    bool line_feed_ = false;
    bool resizing_ = false;
    bool screen_changed_ = false;
};

} // namespace neko
