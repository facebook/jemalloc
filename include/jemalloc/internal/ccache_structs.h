#ifndef JEMALLOC_INTERNAL_CCACHE_STRUCTS_H
#define JEMALLOC_INTERNAL_CCACHE_STRUCTS_H

#include "jemalloc/internal/jemalloc_preamble.h"

struct ccache_stats_s {
	uint64_t nrequests;
};

struct JEMALLOC_ALIGNED(CACHELINE) ccache_s {
	unsigned       ind;
	malloc_mutex_t mtx;
	atomic_u_t     state;
	ccache_stats_t stats;
	/* Lock-free once initialized. */
	cache_bin_t bins[CCACHE_NBINS_MAX];
	/* Note:
	 * each ccache_t * is aligned with CACHELINE, so there could be paddings
	 * after bins.
	 */
};
#endif /* JEMALLOC_INTERNAL_CCACHE_STRUCTS_H */
