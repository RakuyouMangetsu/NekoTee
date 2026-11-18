#pragma once

namespace neko {

class Terminal;
class Session;
class Signals;
class Mirror;

int relay(Terminal& terminal, Session& session, const Signals& signals, Mirror& mirror);

} // namespace neko
