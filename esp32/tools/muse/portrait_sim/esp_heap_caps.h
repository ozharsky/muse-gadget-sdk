/* Host stand-ins for the ESP-IDF calls muse_portrait.c makes (tools/muse/portrait_sim). */
#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
static inline void *heap_caps_malloc(size_t n, int caps) { (void)caps; return malloc(n); }
static inline void *heap_caps_calloc(size_t a, size_t b, int caps) { (void)caps; return calloc(a, b); }
static inline void *heap_caps_aligned_alloc(size_t al, size_t n, int caps)
{
    (void)caps;
    void *p = NULL;
    return posix_memalign(&p, al < sizeof(void *) ? sizeof(void *) : al, n) ? NULL : p;
}
static inline void heap_caps_free(void *p) { free(p); }
