/**
 * @file        kernel/runtime_settings.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <rex/setting.h>
#include <rex/types.h>

namespace rex::kernel {

class REX_KERNEL_API RuntimeSettings final : public SettingOwner {
 public:
  REX_SETTING(u32, heap_size_mb, 256, "First shim heap segment in MiB", Range{4, 2048},
              kLockedInGame);
  REX_SETTING(u32, title_memory_mb, 480, "Title memory in MiB, 0 removes the cap", Range{0, 512},
              kLockedInGame);
  REX_SETTING(bool, writable_code_segments, false, "Load code and read only data writable",
              kLockedInGame);
  REX_SETTING(bool, protect_zero, true, "The first 64 KiB fault on any access", kLockedInGame);
  REX_SETTING(bool, protect_on_release, false, "Released memory faults on access", kLockedInGame);
  REX_SETTING(bool, scribble_heap, false, "Fill new heap memory with a pattern", kLockedInGame);
  REX_SETTING(bool, ignore_thread_priorities, true, "Ignore guest thread priorities");
  REX_SETTING(bool, ignore_thread_affinities, true, "Ignore guest thread affinities");
  REX_SETTING(bool, ffmpeg_verbose, false, "Log FFmpeg messages for XMA decoding");
  REX_SETTING(u32, terminate_timeout_ms, 2000, "Time Unload waits for guest threads to exit",
              Range{0, 60000});
  // Battlefield 1943 Reborn patches (v0.10's guest_invalid_call_skip and job_wait_floor_ms).
  REX_SETTING(bool, guest_invalid_call_skip, false,
              "A guest call to an invalid or unregistered function address is skipped (returning 0) and "
              "logged, instead of stopping the program");
  REX_SETTING(u32, job_wait_floor_ms, 5,
              "The guest's job threads' wait timeouts at least this many ms (0: as the game asks)",
              Range{0, 100});
  // Battlefield 1943 Reborn patch (the SDK v0.11 port, 25 Nov 2026): for an app whose title runs once per process.
  REX_SETTING(bool, keep_guest_memory_at_exit, false,
              "The title's address space is left mapped when the runtime shuts down, for the process's exit to "
              "release: host threads of the app that still read guest memory find it there");

  RuntimeSettings();
};

}  // namespace rex::kernel
