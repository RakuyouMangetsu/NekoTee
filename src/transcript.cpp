#include "transcript.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace neko {
namespace {

int rows_for(int rows) { return std::clamp(rows == 0 ? 24 : rows, 2, 512); }
int columns_for(int cols) { return std::clamp(cols == 0 ? 80 : cols, 2, 512); }

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
std::string visible_row(int columns, ReadCell read) {
    std::string text;
    std::size_t blanks = 0;
    for (int col = 0; col < columns; ++col) {
        const auto cell = read(col);
        if (cell.chars[0] == static_cast<std::uint32_t>(-1)) {
            // The preceding wide glyph owns this continuation cell.
            continue;
        }
        if (cell.attrs.conceal || !visible_codepoint(cell.chars[0])) {
            ++blanks;
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
    return text;
}

} // namespace

Transcript::Transcript(int rows, int columns)
    : vterm_(vterm_new(rows_for(rows), columns_for(columns)), vterm_free),
      screen_(vterm_obtain_screen(vterm_.get())), state_(vterm_obtain_state(vterm_.get())),
      primary_(rows_for(rows)), alternate_(primary_.size()), columns_(columns_for(columns)) {
    static constexpr VTermScreenCallbacks callbacks{
        damage, move, nullptr, property, nullptr, nullptr, scrollback, nullptr, nullptr};
    vterm_set_utf8(vterm_.get(), 1);
    vterm_output_set_callback(vterm_.get(), [](const char*, std::size_t, void*) {}, nullptr);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_reset(screen_, 1);
}

std::string Transcript::feed(std::string_view bytes) {
    // Take snapshots before line feeds so even lines that scroll away in one
    // read are transcribed. VTerm keeps partial escape and UTF-8 state.
    for (auto newline = bytes.find('\n'); newline != std::string_view::npos;
         newline = bytes.find('\n')) {
        input(bytes.substr(0, newline));
        publish();
        VTermPos before{}, after{};
        vterm_state_get_cursorpos(state_, &before);
        const bool blank = active()[before.row].text.empty();
        const auto previous_scroll = scroll_events_;
        input(bytes.substr(newline, 1));
        vterm_state_get_cursorpos(state_, &after);
        if (after.row != before.row || previous_scroll != scroll_events_) {
            if (blank) {
                close_line();
                output_ += "\r\n";
            } else if (open_row_ != after.row) {
                close_line();
            }
        }
        bytes.remove_prefix(newline + 1);
    }
    input(bytes);
    publish();
    return std::exchange(output_, {});
}

void Transcript::input(std::string_view bytes) {
    if (!bytes.empty()) {
        vterm_input_write(vterm_.get(), bytes.data(), bytes.size());
    }
}

std::string Transcript::resize(int rows, int columns) {
    rows = rows_for(rows);
    columns = columns_for(columns);
    if (rows == static_cast<int>(primary_.size()) && columns == columns_) {
        return {};
    }
    publish();
    resizing_ = true;
    vterm_set_size(vterm_.get(), rows, columns);
    resizing_ = false;
    columns_ = columns;
    primary_.resize(rows);
    alternate_.resize(rows);
    damage({0, rows, 0, columns}, this);
    publish();
    return std::exchange(output_, {});
}

int Transcript::damage(VTermRect rect, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    if (!self.resizing_) {
        auto& rows = self.active();
        const int end = std::min(rect.end_row, static_cast<int>(rows.size()));
        for (int row = std::max(0, rect.start_row); row < end; ++row) {
            rows[row].dirty = true;
        }
    }
    return 1;
}

int Transcript::move(VTermRect dest, VTermRect src, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    self.scroll_index_ = 0;
    if (self.resizing_) {
        return 1;
    }
    if (dest.start_col != 0 || src.start_col != 0 || dest.end_col != self.columns_ ||
        src.end_col != self.columns_ || dest.start_row == src.start_row) {
        return damage(dest, user);
    }

    // Keep the reported text attached to the physical row as it scrolls.
    auto& rows = self.active();
    if (self.open_row_ >= src.start_row && self.open_row_ < src.end_row) {
        self.open_row_ += dest.start_row - src.start_row;
    } else if (self.open_row_ >= dest.start_row && self.open_row_ < dest.end_row) {
        self.close_line();
    }
    if (dest.start_row < src.start_row) {
        std::move(rows.begin() + src.start_row, rows.begin() + src.end_row,
                  rows.begin() + dest.start_row);
    } else {
        std::move_backward(rows.begin() + src.start_row, rows.begin() + src.end_row,
                           rows.begin() + dest.end_row);
    }
    for (int row = src.start_row; row < src.end_row; ++row) {
        if (row < dest.start_row || row >= dest.end_row) {
            rows[row] = {};
        }
    }
    ++self.scroll_events_;
    return 1;
}

int Transcript::property(VTermProp prop, VTermValue* value, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    if (prop == VTERM_PROP_ALTSCREEN && self.alternate_active_ != (value->boolean != 0)) {
        self.close_line();
        self.alternate_active_ = value->boolean != 0;
        auto& rows = self.active();
        if (self.alternate_active_) {
            std::fill(rows.begin(), rows.end(), Row{});
        }
        damage({0, static_cast<int>(rows.size()), 0, self.columns_}, user);
    }
    return 1;
}

int Transcript::scrollback(int columns, const VTermScreenCell* cells, void* user) {
    auto& self = *static_cast<Transcript*>(user);
    if (!self.resizing_ && self.scroll_index_ < static_cast<int>(self.active().size())) {
        self.publish_row(self.scroll_index_++,
                         visible_row(columns, [cells](int col) { return cells[col]; }), true);
    }
    return 1;
}

void Transcript::publish() {
    VTermPos cursor{};
    vterm_state_get_cursorpos(state_, &cursor);
    if (open_row_ >= 0 && open_row_ != cursor.row) {
        close_line();
    }
    auto& rows = active();
    for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
        if (std::exchange(rows[row].dirty, false)) {
            auto text = visible_row(columns_, [this, row](int col) {
                VTermScreenCell cell{};
                vterm_screen_get_cell(screen_, {row, col}, &cell);
                return cell;
            });
            publish_row(row, std::move(text), row != cursor.row);
        }
    }
}

void Transcript::publish_row(int row, std::string text, bool complete) {
    auto& previous = active()[row].text;
    if (text != previous) {
        if (text.empty()) {
            if (open_row_ == row) {
                close_line();
            }
        } else if (open_row_ == row && text.compare(0, previous.size(), previous) == 0) {
            output_.append(text, previous.size(), std::string::npos);
        } else {
            close_line();
            output_ += text;
            open_row_ = row;
        }
        previous = std::move(text);
    }
    if (complete && open_row_ == row) {
        close_line();
    }
}

void Transcript::close_line() {
    if (open_row_ >= 0) {
        output_ += "\r\n";
        open_row_ = -1;
    }
}

} // namespace neko
