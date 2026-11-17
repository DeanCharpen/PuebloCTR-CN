/* syscall() on the 3DS (port/whole/3ds/system_3ds.cpp): only SYS_gettid, the id of the calling
 * thread (the runtime names threads in its logs by it); anything else fails with ENOSYS. */
#pragma once

#define SYS_gettid 224

#ifdef __cplusplus
extern "C" {
#endif

long re4_3ds_syscall(long number);
#define syscall(number, ...) re4_3ds_syscall(number)

#ifdef __cplusplus
}
#endif
