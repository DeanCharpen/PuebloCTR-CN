/* Memory mappings on the 3DS (port/whole/3ds/system_3ds.cpp). Anonymous mappings come from the
 * heap, page-aligned and zeroed; a fixed mapping is MEM1 at RE4_MEM1_BASE, which the heap setup
 * keeps out of the C library's heap. Protection is not changed (stacks get no guard pages). */
#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000

#define MAP_FAILED ((void*) -1)

void* mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* address, size_t length);
int mprotect(void* address, size_t length, int prot);

#ifdef __cplusplus
}
#endif
