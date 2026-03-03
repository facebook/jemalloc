#ifndef JEMALLOC_INTERNAL_HPA_CENTRAL_H
#define JEMALLOC_INTERNAL_HPA_CENTRAL_H

#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/base.h"
#include "jemalloc/internal/hpa_hooks.h"
#include "jemalloc/internal/hpdata.h"
#include "jemalloc/internal/mutex.h"
#include "jemalloc/internal/tsd_types.h"

typedef struct hpa_pool_s hpa_pool_t;
struct hpa_pool_s {
	/*
	 * Pool of empty huge pages to be shared between shards that are
	 * participating.
	 *
	 * Page is owned by the pool if it lives in one of these two lists.
	 * This means that it should not be part of any hpa_shard's psset at the
	 * same time.
	 */
	hpdata_empty_list_t nonpurged;
	hpdata_empty_list_t purged;
};

typedef struct hpa_central_stats_s hpa_central_stats_t;
struct hpa_central_stats_s {
	/* Number of pages purged while they were in the central pool */
	uint64_t npurged_pool;

	/* Total number of dirty base pages in the pool */
	size_t ndirty_pool;
};

typedef struct hpa_central_s hpa_central_t;
struct hpa_central_s {
	/* Guards the access to central pool of empty hugepages */
	malloc_mutex_t pool_mtx;
	hpa_pool_t     pool;

	/*
	 * Guards expansion of eden.  We separate this from the regular mutex so
	 * that cheaper operations can still continue while we're doing the OS
	 * call.
	 */
	malloc_mutex_t grow_mtx;
	/*
	 * Either NULL (if empty), or some integer multiple of a
	 * hugepage-aligned number of hugepages.  We carve them off one at a
	 * time to satisfy new pageslab requests.
	 *
	 * Guarded by grow_mtx.
	 */
	void  *eden;
	size_t eden_len;
	/* Source for metadata. */
	base_t *base;

	/* The HPA hooks. */
	hpa_hooks_t hooks;

	/* Stats */
	hpa_central_stats_t stats;
};

bool hpa_central_init(
    hpa_central_t *central, base_t *base, const hpa_hooks_t *hooks);

hpdata_t *hpa_central_extract(tsdn_t *tsdn, hpa_central_t *central, size_t size,
    uint64_t age, bool hugify_eager, bool *oom);

/* Donate empty page to central */
void hpa_central_donate(
    tsdn_t *tsdn, hpa_central_t *central, hpdata_t *ps, const nstime_t *now);
/* Get empty page from central without growing it */
hpdata_t *hpa_central_borrow(tsdn_t *tsdn, hpa_central_t *central);

/* Purge up to max_ps empty pages in the central */
size_t hpa_central_purge(
    tsdn_t *tsdn, hpa_central_t *central, const nstime_t *now, size_t max_ps);

/* Get time in nanoseconds until central pool needs deferred work */
uint64_t hpa_central_time_until_deferred_work(
    tsdn_t *tsdn, hpa_central_t *central);

void hpa_central_prefork(tsdn_t *tsdn, hpa_central_t *central);
void hpa_central_postfork_parent(tsdn_t *tsdn, hpa_central_t *central);
void hpa_central_postfork_child(tsdn_t *tsdn, hpa_central_t *central);

void hpa_central_stats_read(
    tsdn_t *tsdn, hpa_central_t *central, hpa_central_stats_t *stats);

#endif /* JEMALLOC_INTERNAL_HPA_CENTRAL_H */
