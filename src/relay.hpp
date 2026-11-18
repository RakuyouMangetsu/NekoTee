#pragma once

#include <chrono>
#include <optional>

namespace neko {

class Terminal;
class Session;
class Signals;
class Mirror;

struct RelayOptions {
    std::optional<std::chrono::milliseconds> wait;
    bool limit = true;
    unsigned int rate_percent = 99;
    unsigned int resume_percent = 50;
    bool chinese = false;

    double byte_rate(double wire_rate) const { return wire_rate * rate_percent / 100; }
};

int relay(Terminal& terminal, Session& session, const Signals& signals, Mirror& mirror,
          const RelayOptions& options);

} // namespace neko
