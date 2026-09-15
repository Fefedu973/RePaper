#pragma once
#include <chrono>
#include <stdexcept>

// Cooperative guards used by the vendored parser. No formula text enters errors.
namespace rmchat::mathbudget {
using Clock = std::chrono::steady_clock;
struct State { Clock::time_point deadline; int remaining = 100000; int depth = 0; };
inline thread_local State *active = nullptr;
inline void check() {
    if (active && (--active->remaining <= 0 || Clock::now() > active->deadline))
        throw std::runtime_error("Math rendering budget exceeded");
}
class Session {
    State state;
    State *previous;
public:
    explicit Session(int milliseconds) : state{Clock::now() + std::chrono::milliseconds(milliseconds)}, previous(active) { active = &state; }
    ~Session() { active = previous; }
};
class Depth {
    State *state;
public:
    Depth() : state(active) {
        check();
        if (state && state->depth >= 64) throw std::runtime_error("Math nesting limit exceeded");
        if (state) ++state->depth;
    }
    ~Depth() { if (state) --state->depth; }
};
}
