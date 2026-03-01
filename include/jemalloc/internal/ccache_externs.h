#ifndef JEMALLOC_INTERNAL_CCACHE_EXTERNS_H
#define JEMALLOC_INTERNAL_CCACHE_EXTERNS_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/ccache_structs.h"
#include "jemalloc/internal/ccache_types.h"

extern bool opt_ccache;
void        ccache_tdata_cleanup(tsd_t *tsd);
void        ccache_boot(tsdn_t *tsdn, base_t *base);
ccache_t   *ccache_get(tsdn_t *tsdn, unsigned cpu_id);

#ifndef JEMALLOC_RSEQ_SUPPORTED
JEMALLOC_ALWAYS_INLINE void *
ccache_alloc(tsd_t *tsd, szind_t ind, bool zero) {
	not_reached();
	return NULL;
}

JEMALLOC_ALWAYS_INLINE bool
ccache_dalloc(tsd_t *tsd, void *ptr, szind_t ind) {
	not_reached();
}
#else
#	include "jemalloc/internal/ccache_inlines.h"
#endif

#endif /* JEMALLOC_INTERNAL_CCACHE_EXTERNS_H */
