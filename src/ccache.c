#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

JEMALLOC_ALIGNED(CACHELINE)
static ccache_t        *ccaches; /* Read-only after initialization. */
static cache_bin_info_t ccache_bin_info[CCACHE_NBINS_MAX] = {{0}};

bool opt_ccache = false;

typedef enum {
	CCACHE_DISABLED = 0,
	CCACHE_ENABLED = 1,
	CCACHE_INITIALIZED = 2,
	CCACHE_STARTED = 2,
	CCACHE_STOPPED = 3
} ccache_state_t;

static inline ccache_state_t
ccache_state_get_unsafe(ccache_t *ccache) {
	assert(ccache != NULL);
	return (ccache_state_t)atomic_load_u(&ccache->state, ATOMIC_RELAXED);
}

static inline ccache_state_t
ccache_state_get(ccache_t *ccache) {
	assert(ccache != NULL);
	return (ccache_state_t)atomic_load_u(&ccache->state, ATOMIC_ACQUIRE);
}

static inline void
ccache_state_set(ccache_t *ccache, ccache_state_t state) {
	assert(ccache != NULL);
	return atomic_store_u(&ccache->state, (unsigned)state, ATOMIC_RELEASE);
}

static inline bool
ccache_initialized(ccache_t *ccache) {
	assert(ccache != NULL);
	return ccache_state_get_unsafe(ccache) >= CCACHE_INITIALIZED;
}

static inline bool
ccache_init(tsdn_t *tsdn, ccache_t *ccache) {
	assert(ccache != NULL);
	ccache_state_t state = ccache_state_get(ccache);
	if (state == CCACHE_DISABLED) {
		return true;
	}
	if (state >= CCACHE_INITIALIZED) {
		return false;
	}
	assert(state == CCACHE_ENABLED);
	malloc_mutex_lock(tsdn, &ccache->mtx);
	size_t ccache_stack_size, alignment;
	cache_bin_info_compute_alloc(
	    ccache_bin_info, CCACHE_NBINS_MAX, &ccache_stack_size, &alignment);
	void *mem;
	mem = base_alloc(tsdn, b0get(), ccache_stack_size, alignment);
	if (mem == NULL) {
		ccache_state_set(ccache, CCACHE_DISABLED);
		malloc_mutex_unlock(tsdn, &ccache->mtx);
		return true;
	}

	size_t cur_offset = 0;
	cache_bin_preincrement(
	    ccache_bin_info, CCACHE_NBINS_MAX, mem, &cur_offset);
	for (unsigned i = 0; i < CCACHE_NBINS_MAX; i++) {
		cache_bin_t *cache_bin = &ccache->bins[i];
		if (ccache_bin_info[i].ncached_max > 0) {
			cache_bin_init(
			    cache_bin, &ccache_bin_info[i], mem, &cur_offset);
		} else {
			cache_bin_init_disabled(
			    cache_bin, ccache_bin_info[i].ncached_max);
		}
	}
	cache_bin_postincrement(mem, &cur_offset);
	assert(cur_offset == ccache_stack_size);
	ccache_state_set(ccache, CCACHE_INITIALIZED);
	malloc_mutex_unlock(tsdn, &ccache->mtx);
	return false;
}

void
ccache_tdata_cleanup(tsd_t *tsd) {
	assert(have_rseq_support);
	return ccache_tdata_cleanup_impl(tsd);
}

void
ccache_boot(tsdn_t *tsdn, base_t *base) {
	assert(ncpus > 0);
	assert(have_rseq_support && opt_ccache);
	assert(CACHELINE_CEILING(sizeof(ccache_t)) == sizeof(ccache_t));
	ccaches = (ccache_t *)base_alloc(
	    tsdn, base, sizeof(ccache_t) * ncpus, CACHELINE);
	if (unlikely(ccaches == NULL)) {
		opt_ccache = false;
		return;
	}
	for (unsigned i = 0; i < ncpus; i++) {
		ccache_t *ccache = &ccaches[i];
		assert(ALIGNMENT_ADDR2BASE(ccache, CACHELINE) == ccache);
		ccache->ind = i;
		ccache->stats.nrequests = 0;
		if (malloc_mutex_init(&ccache->mtx, "ccache",
		        WITNESS_RANK_CCACHE, malloc_mutex_rank_exclusive)) {
			ccache_state_set(ccache, CCACHE_DISABLED);
		} else {
			ccache_state_set(ccache, CCACHE_ENABLED);
		}
	}
	for (unsigned i = 0; i < CCACHE_NBINS_MAX; i++) {
		cache_bin_info_init(&ccache_bin_info[i], CCACHE_NCACHED_MAX);
	}
}

ccache_t *
ccache_get(tsdn_t *tsdn, unsigned cpu_id) {
	if (ccaches == NULL) {
		assert(!malloc_initialized());
		return NULL;
	}

	assert(cpu_id < ncpus);
	assert(have_rseq_support && opt_ccache);
	ccache_t *ccache = &ccaches[cpu_id];
	if (unlikely(
	        !ccache_initialized(ccache) && ccache_init(tsdn, ccache))) {
		return NULL;
	}

	assert(ccache_initialized(ccache));
	return ccache;
}
