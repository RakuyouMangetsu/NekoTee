#include "transcript.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace neko {
namespace {

int rows_for(int rows) { return std::clamp(rows == 0 ? 24 : rows, 2, 512); }
int columns_for(int cols) { return std::clamp(cols == 0 ? 80 : cols, 2, 512); }
constexpr std::size_t max_wrapped_prefix = 64 * 1024;

bool visible_codepoint(std::uint32_t ch) {
    return ch >= 0x20 && (ch < 0x7F || ch > 0x9F) && ch <= 0x10FFFF &&
           (ch < 0xD800 || ch > 0xDFFF);
}

void utf8(std::string& text, std::uint32_t ch) {
    if (ch < 0x80) {
        text.push_back(static_cast<char>(ch));
        return;
    }
    const int extra = ch < 0x800 ? 1 : ch < 0x10000 ? 2 : 3;
    constexpr unsigned int prefix[] = {0, 0xC0, 0xE0, 0xF0};
    text.push_back(static_cast<char>(prefix[extra] | (ch >> (6 * extra))));
    for (int part = extra - 1; part >= 0; --part) {
        text.push_back(static_cast<char>(0x80U | (ch >> (6 * part) & 0x3FU)));
    }
}

template <class ReadCell>
void append_visible_row(std::string& text, int columns, ReadCell read) {
    std::size_t blanks = 0;
    for (int col = 0; col < columns; ++col) {
        const auto cell = read(col);
        if (cell.chars[0] == static_cast<std::uint32_t>(-1)) {
            // The preceding wide glyph owns this continuation cell.
            continue;
        }
        if (cell.attrs.conceal || !visible_codepoint(cell.chars[0])) {
            blanks += std::max(1, static_cast<int>(cell.width));
            continue;
        }
        text.append(blanks, ' ');
        blanks = 0;
        for (const auto ch : cell.chars) {
            if (ch == 0) {
                break;
            }
            if (visible_codepoint(ch)) {
                utf8(text, ch);
            }
        }
    }
}

} // namespace

Transcript::Transcript(int rows, int columns)
    : vterm_(vterm_new(rows_for(rows), columns_for(columns)), vterm_free),
      screen_(vterm_obtain_screen(vterm_.get())), state_(vterm_obtain_state(vterm_.get())),
      columns_(columns_for(columns)) {
    static constexpr VTermScreenCallbacks callbacks{
        damage, move, nullptr, property, nullptr, nullptr, scrollback, nullptr, nullptr};
    vterm_set_utf8(vterm_.get(), 1);
    vterm_output_set_callback(vterm_.get(), [](const char*, std::size_t, void*) {}, nullptr);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_reset(screen_, 1);
}

std::string Transcript::feed(std::string_view bytes) {
    std::string output;
    for (auto newline = bytes.find('\n'); newline != std::string_view::npos;
         newline = bytes.find('\n')) {
        input(bytes.substr(0, newline));
        VTermPos before{}, after{};
        vterm_state_get_cursorpos(state_, &before);

        // Capture the line before LF can scroll it away. Edits and suggestions
        // only affect the screen model until a line feed reaches the terminal.
        const auto start = output.size();
        append_line(output, before.row);
        screen_changed_ = false;
        line_feed_ = true;
        input(bytes.substr(newline, 1));
        line_feed_ = false;
        vterm_state_get_cursorpos(state_, &after);
        if (after.row != before.row || screen_changed_) {
            output += "\r\n";
            clear_wrapped_prefix();
        } else {
            // For example, libvterm ignores LF inside an SOS control string.
            output.resize(start);
        }
        bytes.remove_prefix(newline + 1);
    }
    input(bytes);
    return output;
}

void Transcript::input(std::string_view bytes) {
    if (!bytes.empty()) {
        vterm_input_write(vterm_.get(), bytes.data(), bytes.size());
    }
}

void Transcript::resize(int rows, int columns) {
    int old_rows = 0;
    vterm_get_size(vterm_.get(), &old_rows, nullptr);
    rows = rows_for(rows);
    columns = columns_for(columns);
    if (rows != old_rows || columns != columns_) {
        clear_wrapped_prefix();
        resizing_ = true;
        vterm_set_size(vterm_.get(), rows, columns);
        resizing_ = false;
        columns_ = columns;
    }
}

int Transcript::damage(VTermRect rect, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    self.screen_changed_ = true;
    if (rect.start_row == 0 && rect.start_col == 0 && rect.end_col == self.columns_) {
        self.clear_wrapped_prefix();
    }
    return 1;
}

int Transcript::move(VTermRect dest, VTermRect src, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    if (dest.start_row != src.start_row && (dest.start_row == 0 || src.start_row == 0)) {
        if (dest.start_row == 0 && src.start_row == 1 && self.scrolled_row_) {
            if (!self.scrolled_row_->empty()) {
                self.wrapped_prefix_bytes_ += self.scrolled_row_->size();
                self.wrapped_prefix_.push_back(std::move(*self.scrolled_row_));
                while (self.wrapped_prefix_bytes_ > max_wrapped_prefix) {
                    self.wrapped_prefix_bytes_ -= self.wrapped_prefix_.front().size();
                    self.wrapped_prefix_.pop_front();
                }
            }
        } else {
            self.clear_wrapped_prefix();
        }
    }
    self.scrolled_row_.reset();
    return 1;
}

int Transcript::property(VTermProp prop, VTermValue*, void* user) {
    if (prop == VTERM_PROP_ALTSCREEN) {
        static_cast<Transcript*>(user)->clear_wrapped_prefix();
    }
    return 1;
}

int Transcript::scrollback(int columns, const VTermScreenCell* cells, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    self.scrolled_row_.reset();
    // The state layer has moved the wrap flags, but the screen is still
    // reporting the old row. A continued new top row needs the evicted text.
    if (!self.line_feed_ && !self.resizing_ &&
        vterm_state_get_lineinfo(self.state_, 0)->continuation) {
        append_visible_row(self.scrolled_row_.emplace(), columns, [cells](int col) { return cells[col]; });
    }
    return 1;
}

void Transcript::clear_wrapped_prefix() {
    wrapped_prefix_.clear();
    wrapped_prefix_bytes_ = 0;
    scrolled_row_.reset();
}

void Transcript::append_line(std::string& output, int last_row) const {
    int first_row = last_row;
    while (first_row > 0 && vterm_state_get_lineinfo(state_, first_row)->continuation) {
        --first_row;
    }
    if (first_row == 0 && vterm_state_get_lineinfo(state_, 0)->continuation) {
        for (const auto& row : wrapped_prefix_) {
            output += row;
        }
    }
    for (int row = first_row; row <= last_row; ++row) {
        append_visible_row(output, columns_, [this, row](int col) {
            VTermScreenCell cell{};
            vterm_screen_get_cell(screen_, {row, col}, &cell);
            return cell;
        });
    }
}

} // namespace neko
