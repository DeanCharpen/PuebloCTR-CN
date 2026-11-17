/* No stack walk on the 3DS (port/whole/3ds): the wait traces of os_thread.cpp stay empty. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

static inline int backtrace(void** frames, int size)
{
    (void) frames;
    (void) size;
    return 0;
}
static inline char** backtrace_symbols(void* const* frames, int size)
{
    (void) frames;
    (void) size;
    return 0;
}
static inline void backtrace_symbols_fd(void* const* frames, int size, int fd)
{
    (void) frames;
    (void) size;
    (void) fd;
}

#ifdef __cplusplus
}
#endif
