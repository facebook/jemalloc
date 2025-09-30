#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/hpa_central.h"

#include "jemalloc/internal/assert.h"
#include "jemalloc/internal/hpa_utils.h"
#include "jemalloc/internal/tsd.h"
#include "jemalloc/internal/witness.h"

#define HPA_EDEN_SIZE (128 * HUGEPAGE)
#define MILLION UINT64_C(1000000)

uint64_t opt_hpa_pool_purge_delay_ms = 10000; /* 10s */

void
hpa_central_pool_init(hpa_pool_t *pool) {
	hpdata_empty_list_init(&pool->nonpurged);
	hpdata_empty_list_init(&pool->purged);
}

void
hpa_central_stats_read(
    tsdn_t *tsdn, hpa_central_t *central, hpa_central_stats_t *stats) {
	malloc_mutex_lock(tsdn, &central->pool_mtx);
	stats->ndirty_pool = central->stats.ndirty_pool;
	stats->npurged_pool = central->stats.npurged_pool;
	malloc_mutex_unlock(tsdn, &central->pool_mtx);
}

static inline void
hpa_central_pool_concat_nonpurged(tsdn_t *tsdn, hpa_central_t *central,
    hpdata_empty_list_t *pages, size_t new_dirty) {
	malloc_mutex_lock(tsdn, &central->pool_mtx);
	hpdata_empty_list_concat(&central->pool.nonpurged, pages);
	central->stats.ndirty_pool += new_dirty;
	malloc_mutex_unlock(tsdn, &central->pool_mtx);
}

static void
hpa_central_get_nonpurged(tsdn_t *tsdn, hpa_central_t *central,
    const nstime_t *now, hpa_purge_batch_t *batch) {
	malloc_mutex_lock(tsdn, &central->pool_mtx);
	while (!hpa_batch_full(batch)
	    && !hpdata_empty_list_empty(&central->pool.nonpurged)) {
		hpdata_t *ps = hpdata_empty_list_first(
		    &central->pool.nonpurged);
		assert(hpdata_empty(ps) && hpdata_purge_allowed_get(ps));

		const nstime_t *allowed = hpdata_time_purge_allowed_get(ps);
		if (nstime_compare(now, allowed) < 0) {
			break;
		}
		hpdata_empty_list_remove(&central->pool.nonpurged, ps);
		assert(batch->item_cnt < batch->items_capacity);
		hpa_purge_item_t *hp_item = &batch->items[batch->item_cnt];
		batch->item_cnt++;
		hp_item->hp = ps;
		hp_item->dehugify = hpdata_huge_get(hp_item->hp);
		size_t nranges;
		hpdata_alloc_allowed_set(hp_item->hp, false);
		size_t ndirty = hpdata_purge_begin(
		    hp_item->hp, &hp_item->state, &nranges);
		assert(ndirty > 0 && nranges > 0);
		batch->ndirty_in_batch += ndirty;
		batch->nranges += nranges;
		batch->npurged_hp_total++;
	}
	malloc_mutex_unlock(tsdn, &central->pool_mtx);
}

static void
hpa_central_put_purged(
    tsdn_t *tsdn, hpa_central_t *central, const hpa_purge_batch_t *batch) {
	assert(batch->item_cnt > 0);
	hpdata_empty_list_t newly_purged;
	hpdata_empty_list_init(&newly_purged);

	for (size_t i = 0; i < batch->item_cnt; ++i) {
		hpa_purge_item_t *hp_item = &batch->items[i];
		if (hp_item->dehugify) {
			hpdata_dehugify(hp_item->hp);
		}
		hpdata_purge_end(hp_item->hp, &hp_item->state);
		hpdata_alloc_allowed_set(hp_item->hp, true);
		hpdata_purge_allowed_set(hp_item->hp, false);
		hpdata_empty_list_append(&newly_purged, hp_item->hp);
	}

	malloc_mutex_lock(tsdn, &central->pool_mtx);
	hpdata_empty_list_concat(&central->pool.purged, &newly_purged);
	central->stats.npurged_pool += batch->npurged_hp_total;
	assert(central->stats.ndirty_pool >= batch->ndirty_in_batch);
	central->stats.ndirty_pool -= batch->ndirty_in_batch;
	malloc_mutex_unlock(tsdn, &central->pool_mtx);
}

void
hpa_central_ps_insert(tsdn_t *tsdn, hpa_central_t *central,
    hpdata_empty_list_t *pages, const nstime_t *now) {
	assert(!hpdata_empty_list_empty(pages));

	assert(now != NULL);
	nstime_t purge_time;
	nstime_copy(&purge_time, now);
	uint64_t purge_delay_ns = opt_hpa_pool_purge_delay_ms * MILLION;
	nstime_iadd(&purge_time, purge_delay_ns);

	hpdata_t *ps;
	size_t    new_dirty = 0;
	ql_foreach (ps, &pages->head, ql_link_empty) {
		assert(hpdata_empty(ps));
		assert(hpdata_ndirty_get(ps) > 0);
		hpdata_time_purge_allowed_set(ps, &purge_time);
		new_dirty += hpdata_ndirty_get(ps);
	}
	hpa_central_pool_concat_nonpurged(tsdn, central, pages, new_dirty);
}

hpdata_t *
hpa_central_ps_pop(tsdn_t *tsdn, hpa_central_t *central) {
	hpdata_t *ps = NULL;

	malloc_mutex_lock(tsdn, &central->pool_mtx);
	if (!hpdata_empty_list_empty(&central->pool.nonpurged)) {
		ps = hpdata_empty_list_first(&central->pool.nonpurged);
		hpdata_empty_list_remove(&central->pool.nonpurged, ps);
	}
	if (ps == NULL && !hpdata_empty_list_empty(&central->pool.purged)) {
		ps = hpdata_empty_list_first(&central->pool.purged);
		hpdata_empty_list_remove(&central->pool.purged, ps);
	}
	malloc_mutex_unlock(tsdn, &central->pool_mtx);

	return ps;
}

bool
hpa_central_init(
    hpa_central_t *central, base_t *base, const hpa_hooks_t *hooks) {
	/* malloc_conf processing should have filtered out these cases. */
	assert(hpa_supported());
	bool err;
	err = malloc_mutex_init(&central->grow_mtx, "hpa_central_grow",
	    WITNESS_RANK_HPA_CENTRAL_GROW, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}

	err = malloc_mutex_init(&central->pool_mtx, "hpa_central_pool",
	    WITNESS_RANK_HPA_CENTRAL_POOL, malloc_mutex_rank_exclusive);
	if (err) {
		return true;
	}
	hpa_central_pool_init(&central->pool);

	central->base = base;
	central->eden = NULL;
	central->eden_len = 0;
	central->hooks = *hooks;
	central->stats.npurged_pool = 0;
	central->stats.ndirty_pool = 0;
	return false;
}

static hpdata_t *
hpa_alloc_ps(tsdn_t *tsdn, hpa_central_t *central) {
	return (hpdata_t *)base_alloc(
	    tsdn, central->base, sizeof(hpdata_t), CACHELINE);
}

hpdata_t *
hpa_central_extract(tsdn_t *tsdn, hpa_central_t *central, size_t size,
    uint64_t age, bool hugify_eager, bool *oom) {
	/* Don't yet support big allocations; these should get filtered out. */
	assert(size <= HUGEPAGE);
	/*
	 * Should only try to extract from the central allocator if the local
	 * shard is exhausted.  We should hold the grow_mtx on that shard.
	 */
	witness_assert_positive_depth_to_rank(
	    tsdn_witness_tsdp_get(tsdn), WITNESS_RANK_HPA_SHARD_GROW);

	malloc_mutex_lock(tsdn, &central->grow_mtx);
	*oom = false;

	hpdata_t *ps = NULL;
	bool      start_as_huge = hugify_eager
	    || (init_system_thp_mode == system_thp_mode_always
	        && opt_experimental_hpa_start_huge_if_thp_always);

	/* Is eden a perfect fit? */
	if (central->eden != NULL && central->eden_len == HUGEPAGE) {
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		hpdata_init(ps, central->eden, age, start_as_huge);
		central->eden = NULL;
		central->eden_len = 0;
		malloc_mutex_unlock(tsdn, &central->grow_mtx);
		return ps;
	}

	/*
	 * We're about to try to allocate from eden by splitting.  If eden is
	 * NULL, we have to allocate it too.  Otherwise, we just have to
	 * allocate an edata_t for the new psset.
	 */
	if (central->eden == NULL) {
		/* Allocate address space, bailing if we fail. */
		void *new_eden = central->hooks.map(HPA_EDEN_SIZE);
		if (new_eden == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		if (hugify_eager) {
			central->hooks.hugify(
			    new_eden, HPA_EDEN_SIZE, /* sync */ false);
		}
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			central->hooks.unmap(new_eden, HPA_EDEN_SIZE);
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
		central->eden = new_eden;
		central->eden_len = HPA_EDEN_SIZE;
	} else {
		/* Eden is already nonempty; only need an edata for ps. */
		ps = hpa_alloc_ps(tsdn, central);
		if (ps == NULL) {
			*oom = true;
			malloc_mutex_unlock(tsdn, &central->grow_mtx);
			return NULL;
		}
	}
	assert(ps != NULL);
	assert(central->eden != NULL);
	assert(central->eden_len > HUGEPAGE);
	assert(central->eden_len % HUGEPAGE == 0);
	assert(HUGEPAGE_ADDR2BASE(central->eden) == central->eden);

	hpdata_init(ps, central->eden, age, start_as_huge);

	char *eden_char = (char *)central->eden;
	eden_char += HUGEPAGE;
	central->eden = (void *)eden_char;
	central->eden_len -= HUGEPAGE;

	malloc_mutex_unlock(tsdn, &central->grow_mtx);

	return ps;
}

size_t
hpa_central_purge(
    tsdn_t *tsdn, hpa_central_t *central, const nstime_t *now, size_t max_ps) {
	VARIABLE_ARRAY(hpa_purge_item_t, items, HPA_PURGE_BATCH_MAX);
	hpa_purge_batch_t batch = {
	    .max_hp = max_ps,
	    .npurged_hp_total = 0,
	    .items = &items[0],
	    .items_capacity = HPA_PURGE_BATCH_MAX,
	    .range_watermark = hpa_process_madvise_max_iovec_len(),
	};
	assert(batch.range_watermark > 0);

	do {
		hpa_batch_pass_start(&batch);
		assert(hpa_batch_empty(&batch));
		hpa_central_get_nonpurged(tsdn, central, now, &batch);
		if (hpa_batch_empty(&batch)) {
			break;
		}
		/* We don't need any lock while purging pages from the pool. */
		hpa_purge_batch(&central->hooks, batch.items, batch.item_cnt);
		hpa_central_put_purged(tsdn, central, &batch);
	} while (hpa_batch_full(&batch));
	return batch.npurged_hp_total;
}

/*
 *No need to do any of below for central->grow_mtx as shard->grow_mtx must be
 * held to lock that one.
 */
void
hpa_central_prefork(tsdn_t *tsdn, hpa_central_t *central) {
	malloc_mutex_prefork(tsdn, &central->pool_mtx);
}

void
hpa_central_postfork_parent(tsdn_t *tsdn, hpa_central_t *central) {
	malloc_mutex_postfork_parent(tsdn, &central->pool_mtx);
}

void
hpa_central_postfork_child(tsdn_t *tsdn, hpa_central_t *central) {
	malloc_mutex_postfork_child(tsdn, &central->pool_mtx);
}
