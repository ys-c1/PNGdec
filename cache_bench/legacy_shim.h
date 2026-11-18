//
// legacy_shim.h - no-op stand-ins for the cache API used when the benchmark
// is compiled against the pre-change library (HEAD's original png.inl, which
// has no header cache). Only enabled with -DPNG_CACHE_LEGACY.
//
#ifdef PNG_CACHE_LEGACY
#ifndef PNG_CACHE_LEGACY_SHIM_H
#define PNG_CACHE_LEGACY_SHIM_H

#ifndef PNG_CACHE_LRU
#define PNG_CACHE_LRU 0
#define PNG_CACHE_FIFO 1
#endif

extern "C" {
static inline int PNG_cache_configure(int, int, int) { return PNG_SUCCESS; }
static inline void PNG_cache_reset(void) {}
static inline int PNG_cache_stats(int *h, int *m, int *p, int *e, int *n, int *c)
{
    if (h) *h = 0;
    if (m) *m = 0;
    if (p) *p = 0;
    if (e) *e = 0;
    if (n) *n = 0;
    if (c) *c = 0;
    return PNG_SUCCESS;
}
}

#endif // PNG_CACHE_LEGACY_SHIM_H
#endif // PNG_CACHE_LEGACY
