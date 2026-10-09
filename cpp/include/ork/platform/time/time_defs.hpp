#pragma once
#include <chrono>
#include <cstdint>

using TimePoint = int64_t; // microseconds since clock start
using Duration = int64_t;  // microseconds
using Clock = std::chrono::steady_clock;
