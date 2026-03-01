#ifndef JEMALLOC_INTERNAL_CCACHE_INLINES_H
#define JEMALLOC_INTERNAL_CCACHE_INLINES_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/rseq.h"

static inline void
ccache_tdata_cleanup_impl(tsd_t *tsd) {
	assert(have_rseq_support);
	ccache_tdata_t *tdata = tsd_ccache_tdatap_get(tsd);
	if (tdata != NULL) {
		if (tdata->state == CCACHE_REGISTERED) {
			assert(opt_ccache);
			rseq_unregister(tdata->rseq_abi, &tdata->rseq);
		}
		tdata->state = CCACHE_UNREGISTERED;
		tdata->ccache = NULL;
		tdata->rseq_abi = NULL;
		for (unsigned i = 0; i < CCACHE_NBINS_MAX; i++) {
			(tdata->ccache_tstats[i]).nrequests = 0;
		}
	}
}

static inline bool
ccache_ctx_ready(ccache_tdata_t *ctx) {
	assert(ctx != NULL);
	/* The current thread hasn't tried with RSEQ registration. */
	if (unlikely(!ctx->state)) {
		assert(ctx->rseq_abi == NULL);
		for (unsigned i = 0; i < CCACHE_NBINS_MAX; i++) {
			assert((ctx->ccache_tstats[i]).nrequests == 0);
		}
		ctx->rseq_abi = rseq_register(&ctx->rseq);
		ctx->state = ctx->rseq_abi ? CCACHE_REGISTERED
		                           : CCACHE_REGISTER_FAILED;
	}
	assert(ctx->state);
	return ctx->state == CCACHE_REGISTERED;
}

JEMALLOC_ALWAYS_INLINE void *
ccache_alloc_fast(ccache_t *ccache, rseq_t *rseq_abi, szind_t binind) {
	assert(ccache != NULL && rseq_abi != NULL);
	return rseq_alloc(ccache->ind, &ccache->bins[binind], rseq_abi);
}

JEMALLOC_ALWAYS_INLINE bool
ccache_dalloc_fast(
    ccache_t *ccache, rseq_t *rseq_abi, szind_t binind, void *ptr) {
	assert(ccache != NULL && rseq_abi != NULL);
	return rseq_dalloc(ccache->ind, &ccache->bins[binind], rseq_abi, ptr);
}

JEMALLOC_ALWAYS_INLINE void *
ccache_alloc_impl(
    tsdn_t *tsdn, ccache_tdata_t *ctx, szind_t binind, bool *need_refill) {
	void    *ret;
	unsigned cpu_id;
	if (unlikely(!ctx->ccache)) {
		goto slow_path;
	}

	ret = ccache_alloc_fast(ctx->ccache, ctx->rseq_abi, binind);
	if (likely(ret != NULL)) {
		return ret;
	}

slow_path:
	/*
	 * Possible cases:
	 * 1. Thread migrated to a new cpu other than the cached one.
	 * 2. ccache has not been initialized or is disabled.
	 * 3. cache bin is empty.
	 */
	cpu_id = rseq_cpu_id_start_get(ctx->rseq_abi);
	ctx->ccache = ccache_get(tsdn, cpu_id);
	/* The ccache from the cpu id is disabled. */
	if (unlikely(!ctx->ccache)) {
		return NULL;
	}
	ret = ccache_alloc_fast(ctx->ccache, ctx->rseq_abi, binind);
	if (unlikely(ret == NULL)) {
		*need_refill = (cpu_id == rseq_cpu_id_start_get(ctx->rseq_abi));
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE bool
ccache_dalloc_impl(tsdn_t *tsdn, ccache_tdata_t *ctx, szind_t binind, void *ptr,
    bool *need_flush) {
	bool     ret;
	unsigned cpu_id;
	if (unlikely(!ctx->ccache)) {
		goto slow_path;
	}
	ret = ccache_dalloc_fast(ctx->ccache, ctx->rseq_abi, binind, ptr);
	if (likely(!ret)) {
		return ret;
	}

slow_path:
	/*
	 * Possible cases:
	 * 1. Thread migrated to a new cpu other than the cached one.
	 * 2. ccache has not been initialized or is disabled.
	 * 3. cache bin is full.
	 */
	cpu_id = rseq_cpu_id_start_get(ctx->rseq_abi);
	ctx->ccache = ccache_get(tsdn, cpu_id);
	/* The ccache from the cpu id is disabled. */
	if (unlikely(!ctx->ccache)) {
		return true;
	}
	ret = ccache_dalloc_fast(ctx->ccache, ctx->rseq_abi, binind, ptr);
	if (unlikely(ret)) {
		*need_flush = (cpu_id == rseq_cpu_id_start_get(ctx->rseq_abi));
	}
	return ret;
}

JEMALLOC_ALWAYS_INLINE void
ccache_flush(tsd_t *tsd, ccache_tdata_t *ctx, szind_t ind) {
	szind_t  binind = ind - SC_NBINS;
	unsigned nflush = CCACHE_NCACHED_MAX >> 1, npopped = 0;

	VARIABLE_ARRAY(void *, ptrs, nflush + 1);
	while (npopped < nflush) {
		ptrs[npopped] = ccache_alloc_fast(
		    ctx->ccache, ctx->rseq_abi, binind);
		if (ptrs[npopped] == NULL) {
			break;
		}
		npopped++;
	}
	if (unlikely(npopped == 0)) {
		return;
	}
	cache_bin_ptr_array_t arr;
	arr.ptr = ptrs;
	arr.n = npopped;
	arena_ptr_array_flush(tsd, ind, &arr, npopped, /* small */ false,
	    /* stats_arena */ tsd_arena_get(tsd),
	    /* merge_stats */ ctx->ccache_tstats[binind]);
	if (config_stats) {
		(ctx->ccache_tstats[binind]).nrequests = 0;
	}
}

JEMALLOC_ALWAYS_INLINE void *
ccache_alloc(tsd_t *tsd, szind_t ind, bool zero) {
	assert(have_rseq_support && opt_ccache);
	assert(ind >= SC_NBINS && ind < SC_NBINS + CCACHE_NBINS_MAX);

	ccache_tdata_t *ctx = tsd_ccache_tdatap_get(tsd);
	if (unlikely(!ccache_ctx_ready(ctx))) {
		return NULL;
	}

	bool  need_refill = false;
	void *ret = ccache_alloc_impl(
	    tsd_tsdn(tsd), ctx, /* binind */ ind - SC_NBINS, &need_refill);
	if (unlikely(need_refill)) {
		assert(ret == NULL);
	}
	if (unlikely(zero && ret != NULL)) {
		memset(ret, 0, sz_index2size(ind));
	}

	return ret;
}

JEMALLOC_ALWAYS_INLINE bool
ccache_dalloc(tsd_t *tsd, void *ptr, szind_t ind) {
	assert(have_rseq_support && opt_ccache);
	assert(ind >= SC_NBINS && ind < SC_NBINS + CCACHE_NBINS_MAX);

	ccache_tdata_t *ctx = tsd_ccache_tdatap_get(tsd);
	if (unlikely(!ccache_ctx_ready(ctx))) {
		return true;
	}

	bool need_flush = false;
	bool ret = ccache_dalloc_impl(
	    tsd_tsdn(tsd), ctx, /* binind */ ind - SC_NBINS, ptr, &need_flush);
	if (unlikely(need_flush)) {
		assert(ret);
		ccache_flush(tsd, ctx, ind);
	}
	return ret;
}

#endif /* JEMALLOC_INTERNAL_CCACHE_INLINES_H */
