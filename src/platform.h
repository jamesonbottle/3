#pragma once
// HANDOFF [TODO] platform.h - OS abstraction, declarations only.
// Source:  engine.h 91-163 (namespace plat). Declare with unchanged signatures: install_ctrlc(std::atomic<bool>*), ncpu(), pin(int), avail_phys(),
//          total_phys(), lower_priority(), alloc(u64 bytes, bool large, bool& got, bool& locked), release(void*, u64).
//          now() (159-162) is OS-independent: define it `inline` here, not in the .cpp files.
// Needs:   types.h, <atomic>, <chrono>. No <windows.h> or POSIX headers in this header.
// Delete this block when done.
