// High-resolution clocks and sleeps. Windows' default clocks/timeouts tick at 15.6 ms, far too
// coarse for the game's audio mixer and frame pacing.
#pragma once
#include <chrono>
#include <cstdint>

namespace timing {

using steady = std::chrono::steady_clock;

// Game clock (ns since start). Normally real time; set_time_scale() makes it run faster.
int64_t game_ns();
void set_time_scale(double scale);
double time_scale();
// Nanoseconds since the Unix epoch on the game clock, with steady-clock resolution.
int64_t realtime_ns();
// Converts a guest CLOCK_REALTIME absolute deadline to a steady-clock time point.
steady::time_point from_realtime(int64_t deadline_ns);
// Sleeps with sub-millisecond accuracy.
void sleep_ns(int64_t ns);

}  // namespace timing
