/* The 3DS has no dynamic loader (port/whole/3ds). The modules are in the program image
 * (RE4_STATIC_MODULES); what else uses these is diagnostics (dladdr: where an address lies) or
 * Android-only lookups, and finds nothing here. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100

typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;

static inline void* dlopen(const char* file, int mode)
{
    (void) file;
    (void) mode;
    return 0;
}
static inline void* dlsym(void* handle, const char* name)
{
    (void) handle;
    (void) name;
    return 0;
}
static inline int dlclose(void* handle)
{
    (void) handle;
    return 0;
}
static inline char* dlerror(void)
{
    return (char*) "no dynamic loader on the 3DS";
}
static inline int dladdr(const void* address, Dl_info* info)
{
    (void) address;
    (void) info;
    return 0;
}

#ifdef __cplusplus
}
#endif
