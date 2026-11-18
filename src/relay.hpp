#pragma once

#include <chrono>
#include <optional>

namespace neko {

class Terminal;
class Session;
class Signals;
class Mirror;

inline constexpr double unbuffered_rate_fraction = 0.99;

struct RelayOptions {
    std::optional<std::chrono::milliseconds> wait{std::chrono::milliseconds{500}};
    bool limit = false;
    unsigned int resume_percent = 50;
    bool chinese = false;
};

int relay(Terminal& terminal, Session& session, const Signals& signals, Mirror& mirror,
          const RelayOptions& options);

} // namespace neko
