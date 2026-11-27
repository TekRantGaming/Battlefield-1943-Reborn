/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

namespace rex::chrono {

#ifndef __APPLE__
// The host clock the guest's time is made from: CLOCK_MONOTONIC, not CLOCK_MONOTONIC_RAW (Battlefield 1943 Reborn,
// 15 Nov 2026). On some virtual machines the raw clock is not continuous: under WSL 2 it ran 0.45 s ahead of the
// system's own monotonic clock within 5 s and kept stepping (1.09 to 1.04 times as fast over 5 to 20 s), while on a
// KVM VPS the two agreed to 1 ppm. Each step made the game's frame timer, which follows absolute deadlines in guest
// time and starts again from "now" when it finds itself far behind, skip that stretch: a 60 Hz dedicated server ran
// 55.4 ticks a second against its players' 60. CLOCK_MONOTONIC is continuous, runs at real time (NTP's small slew at
// most) and is the clock every wait and std::chrono::steady_clock in the runtime already use, as Windows' QPC is there.
constexpr clockid_t kHostClock = CLOCK_MONOTONIC;
#endif

uint64_t Clock::host_tick_frequency_platform() {
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#else
  timespec res;
  int error = clock_getres(kHostClock, &res);
  assert_zero(error);
  assert_zero(res.tv_sec);  // Sub second resolution is required.

  // Convert nano seconds to hertz. Resolution is 1ns on most systems.
  return 1000000000ull / res.tv_nsec;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#ifdef __APPLE__
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(kHostClock) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#else
  timespec tp;
  int error = clock_gettime(kHostClock, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
