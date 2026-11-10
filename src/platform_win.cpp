// HANDOFF [TODO] platform_win.cpp - Windows implementation of platform.h.
// Source:  engine.h 95-137 (the _WIN32 branch: ctrl, install_ctrlc, ncpu, pin across processor groups, avail/total_phys, lower_priority,
//          lock_priv, alloc, release) and the `#pragma comment(lib, "advapi32.lib")` at 10-12.
// Notes:   wrap the whole file in #ifdef _WIN32. Define NOMINMAX + WIN32_LEAN_AND_MEAN before <windows.h> (engine.h 32-36).
//          g_stop and lock_priv stay file-local statics.
// Preserve: alloc() tries large pages (SeLockMemoryPrivilege) first, then normal pages + VirtualLock; `got`/`locked` mean exactly what they mean today.
// Delete this block when done.
