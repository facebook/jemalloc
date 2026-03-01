#ifndef JEMALLOC_INTERNAL_CCACHE_TYPES_H
#define JEMALLOC_INTERNAL_CCACHE_TYPES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/cache_bin.h"
#include "jemalloc/internal/sc.h"

#define CCACHE_NBINS_MAX                                                       \
	(SC_NGROUP * (LG_USIZE_GROW_SLOW_THRESHOLD - SC_LG_LARGE_MINCLASS) + 1)

typedef struct ccache_stats_s ccache_stats_t;
typedef struct ccache_s       ccache_t;

/* Per-thread data. */
#define CCACHE_TDATA_ZERO_INITIALIZER {0}

typedef enum {
	CCACHE_UNREGISTERED = 0,
	CCACHE_REGISTER_FAILED = 1,
	CCACHE_REGISTERED = 2
} ccache_tdata_state_t;

#ifndef JEMALLOC_RSEQ_SUPPORTED
typedef struct {
	ccache_tdata_state_t state;
	cache_bin_stats_t    ccache_tstats[0];
} ccache_tdata_t;
#else /* JEMALLOC_RSEQ_SUPPORTED */

#	ifdef JEMALLOC_RSEQ_GLIBC
#		include <sys/rseq.h>
#	else
#		include <linux/rseq.h>
#	endif

#	define CCACHE_NCACHED_MAX 20

typedef struct rseq rseq_t;
typedef struct {
	ccache_tdata_state_t state;
	cache_bin_stats_t    ccache_tstats[CCACHE_NBINS_MAX];
	ccache_t            *ccache;
	rseq_t              *rseq_abi;
	rseq_t               rseq;
} ccache_tdata_t;
#endif

#endif /* JEMALLOC_INTERNAL_CCACHE_TYPES_H */
