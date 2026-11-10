// HANDOFF [TODO] platform_posix.cpp - Linux/POSIX implementation of platform.h.
// Source:  engine.h 139-157 (the #else branch: ctrl via SIGINT, ncpu, pin via pthread_setaffinity_np, avail/total_phys via sysconf, no-op lower_priority,
//          alloc = mmap + madvise(MADV_HUGEPAGE) + mlock, release = munmap).
// Notes:   wrap the whole file in #ifndef _WIN32. CPU_SET needs _GNU_SOURCE (g++ defines it; add it at the top if another compiler is used).
//          The `large` argument of alloc() is ignored here, as today.
// Delete this block when done.
