// Crash reporting: on a host crash or abort, writes what happened to the log (and a minidump on
// Windows) so silent exits can be diagnosed.
#pragma once
#include <string>

namespace crash {
void install(const std::string& dir);  // dir = where crash.dmp goes
}
