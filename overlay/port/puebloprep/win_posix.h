// PuebloPrep for Windows (mingw-w64): the POSIX calls of CubeEvil4's RVZ reader (disc_rvz.cpp) - pread, pwrite, fsync -
// on the CRT's; force-included into that file only (puebloprep_build_win.sh: -include win_posix.h). Files in binary mode:
// PuebloPrep's main sets _fmode (open() without O_BINARY would be text mode).
#pragma once
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <sys/types.h>

static inline long long re4_pread(int fd, void* p, size_t n, long long at)
{
    return _lseeki64(fd, at, SEEK_SET) < 0 ? -1 : _read(fd, p, (unsigned) n);
}
static inline long long re4_pwrite(int fd, const void* p, size_t n, long long at)
{
    return _lseeki64(fd, at, SEEK_SET) < 0 ? -1 : _write(fd, p, (unsigned) n);
}
#define pread(fd, p, n, at) re4_pread((fd), (p), (n), (long long) (at))
#define pwrite(fd, p, n, at) re4_pwrite((fd), (p), (n), (long long) (at))
#define fsync _commit
#endif
