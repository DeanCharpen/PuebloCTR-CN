/* dl_iterate_phdr on the 3DS (port/whole/3ds/system_3ds.cpp): one object, the program image
 * [__start__, __end__) as "libre4_main", one loadable, writable segment. The users ask whether an
 * address lies in the game's code or static data (mem1.cpp, os_thread.cpp re4_valid_port_ptr). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} Elf32_Phdr;

#define ElfW(type) Elf32_##type
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

struct dl_phdr_info {
    uintptr_t dlpi_addr;
    const char* dlpi_name;
    const Elf32_Phdr* dlpi_phdr;
    uint16_t dlpi_phnum;
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info* info, size_t size, void* data), void* data);

#ifdef __cplusplus
}
#endif
