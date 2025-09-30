#include "test/jemalloc_test.h"

#include "jemalloc/internal/hpa.h"
#include "jemalloc/internal/nstime.h"

#define SHARD_IND 111
#define SHARD_IND2 112

#define ALLOC_MAX (HUGEPAGE)

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
         * Must be the first member -- we convert back and forth between the
         * test_data_t and the hpa_shard_t;
         */
	hpa_shard_t   shard;
	hpa_central_t central;
	base_t       *base;
	edata_cache_t shard_edata_cache;

	emap_t emap;
};

static hpa_shard_opts_t test_hpa_shard_opts_default = {
    /* slab_max_alloc */
    ALLOC_MAX,
    /* hugification_threshold */
    HUGEPAGE,
    /* dirty_mult */
    FXP_INIT_PERCENT(25),
    /* deferral_allowed */
    false,
    /* hugify_delay_ms */
    10 * 1000,
    /* hugify_sync */
    false,
    /* min_purge_interval_ms */
    0,
    /* experimental_max_purge_nhp */
    -1,
    /* purge_threshold */
    HUGEPAGE,
    /* min_purge_delay_ms */
    0,
    /* hugify_style */
    hpa_hugify_style_eager,
    /* use_pool */
    true};

static hpa_shard_t *
create_test_data(
    hpa_central_t *central, hpa_shard_opts_t *opts, unsigned int shard_ind) {
	bool    err;
	base_t *base = base_new(TSDN_NULL, /* ind */ shard_ind,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(base, "");

	test_data_t *test_data = malloc(sizeof(test_data_t));
	assert_ptr_not_null(test_data, "");

	test_data->base = base;

	err = edata_cache_init(&test_data->shard_edata_cache, base);
	assert_false(err, "");

	err = emap_init(&test_data->emap, test_data->base, /* zeroed */ false);
	assert_false(err, "");

	err = hpa_shard_init(&test_data->shard, central, &test_data->emap,
	    test_data->base, &test_data->shard_edata_cache, shard_ind, opts);
	assert_false(err, "");

	return (hpa_shard_t *)test_data;
}

static void
destroy_test_data(hpa_shard_t *shard) {
	test_data_t *test_data = (test_data_t *)shard;
	base_delete(TSDN_NULL, test_data->base);
	free(test_data);
}

static uintptr_t defer_bump_ptr = HUGEPAGE * 123;
static void *
defer_test_map(size_t size) {
	void *result = (void *)defer_bump_ptr;
	defer_bump_ptr += size;
	return result;
}

static void
defer_test_unmap(void *ptr, size_t size) {
	(void)ptr;
	(void)size;
}

static size_t ndefer_purge_calls = 0;
static size_t npurge_size = 0;
static void
defer_test_purge(void *ptr, size_t size) {
	(void)ptr;
	npurge_size = size;
	++ndefer_purge_calls;
}

static bool defer_vectorized_purge_called = false;
static bool
defer_vectorized_purge(void *vec, size_t vlen, size_t nbytes) {
	(void)vec;
	(void)nbytes;
	++ndefer_purge_calls;
	defer_vectorized_purge_called = true;
	return false;
}

static size_t ndefer_hugify_calls = 0;
static bool
defer_test_hugify(void *ptr, size_t size, bool sync) {
	++ndefer_hugify_calls;
	return false;
}

static size_t ndefer_dehugify_calls = 0;
static void
defer_test_dehugify(void *ptr, size_t size) {
	++ndefer_dehugify_calls;
}

static nstime_t defer_curtime;
static void
defer_test_curtime(nstime_t *r_time, bool first_reading) {
	*r_time = defer_curtime;
}

static uint64_t
defer_test_ms_since(nstime_t *past_time) {
	return (nstime_ns(&defer_curtime) - nstime_ns(past_time)) / 1000 / 1000;
}

TEST_BEGIN(test_central_pool) {
	test_skip_if(!hpa_supported() || !config_stats);

	hpa_hooks_t hooks;
	hooks.map = &defer_test_map;
	hooks.unmap = &defer_test_unmap;
	hooks.purge = &defer_test_purge;
	hooks.hugify = &defer_test_hugify;
	hooks.dehugify = &defer_test_dehugify;
	hooks.curtime = &defer_test_curtime;
	hooks.ms_since = &defer_test_ms_since;
	hooks.vectorized_purge = &defer_vectorized_purge;

	hpa_shard_opts_t opts = test_hpa_shard_opts_default;
	opts.deferral_allowed = true;
	opts.purge_threshold = HUGEPAGE;
	opts.min_purge_delay_ms = 0;
	opts.min_purge_interval_ms = 0;

	hpa_central_t central;
	base_t       *central_base = base_new(TSDN_NULL, /* ind */ 1234,
	          &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(central_base, "");
	hpa_central_init(&central, central_base, &hooks);
	ndefer_purge_calls = 0;
	hpa_shard_t *shard1 = create_test_data(&central, &opts, SHARD_IND);
	hpa_shard_t *shard2 = create_test_data(&central, &opts, SHARD_IND2);

	bool deferred_work_generated = false;
	nstime_init(&defer_curtime, 10 * 1000 * 1000);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	enum { NALLOCS = HUGEPAGE_PAGES };
	edata_t *edatas[NALLOCS];
	for (int i = 0; i < NALLOCS / 2; i++) {
		edatas[i] = pai_alloc(tsdn, &shard1->pai, PAGE, PAGE, false,
		    false, false, &deferred_work_generated);
		expect_ptr_not_null(edatas[i], "Unexpected null edata");
	}
	/* Remember the page */
	hpdata_t *ps = psset_pick_alloc(&shard1->psset, PAGE);
	expect_true(hpdata_huge_get(ps), "Should be huge as we start as huge");

	/* Deallocate all */
	for (int i = 0; i < NALLOCS / 2; i++) {
		pai_dalloc(
		    tsdn, &shard1->pai, edatas[i], &deferred_work_generated);
	}
	hpa_shard_do_deferred_work(tsdn, shard1);
	expect_true(deferred_work_generated, "");
	expect_zu_eq(
	    0, ndefer_purge_calls, "Should donate, not purge delay=0ms");

	/* Stats should not include the page */
	expect_zu_eq(shard1->psset.stats.merged.nactive, 0, "");
	expect_zu_eq(shard1->psset.stats.merged.npageslabs, 0, "Non huge");
	npurge_size = 0;

	/* Make allocation on second shard */
	edata_t *edata2 = pai_alloc(tsdn, &shard2->pai, PAGE, PAGE, false,
	    false, false, &deferred_work_generated);
	expect_ptr_not_null(edata2, "Unexpected null edata");
	expect_zu_eq(shard2->psset.stats.merged.nactive, 1, "");
	hpdata_t *ps2 = psset_pick_alloc(&shard2->psset, PAGE);
	expect_ptr_eq(
	    ps, ps2, "Expected to get the same page via central pool");
	expect_true(hpdata_huge_get(ps2), "Should still be huge");

	expect_zu_eq(shard2->psset.stats.merged.npageslabs, 1, "");
	pai_dalloc(tsdn, &shard2->pai, edata2, &deferred_work_generated);
	expect_true(deferred_work_generated, "");
	ndefer_purge_calls = 0;
	npurge_size = 0;
	hpa_shard_do_deferred_work(tsdn, shard1);
	expect_zu_eq(0, ndefer_purge_calls, "No purge, no donate, delay==0ms");
	hpa_shard_do_deferred_work(tsdn, shard2);
	expect_zu_eq(0, ndefer_purge_calls, "No purge, yes donate, delay==0ms");

	/* Move the time above hard coded limit of 10s */
	nstime_iadd(&defer_curtime, UINT64_C(30) * 1000 * 1000 * 1000);
	hpa_shard_do_deferred_work(tsdn, shard2);
	expect_zu_eq(1, ndefer_purge_calls, "Purged, delay==0ms");
	expect_zu_eq(HUGEPAGE, npurge_size, "Should purge full folio");
	expect_zu_eq(shard1->psset.stats.merged.npageslabs, 0, "");
	expect_zu_eq(shard2->psset.stats.merged.npageslabs, 0, "");
	/* now alloc again and still get the same page */
	edata2 = pai_alloc(tsdn, &shard2->pai, PAGE, PAGE, false, false, false,
	    &deferred_work_generated);
	expect_ptr_not_null(edata2, "Unexpected null edata");
	expect_zu_eq(shard2->psset.stats.merged.nactive, 1, "");
	ps2 = psset_pick_alloc(&shard2->psset, PAGE);
	expect_ptr_eq(
	    ps, ps2, "Expected to get the same page via central pool");
	expect_zu_eq(shard2->psset.stats.merged.npageslabs, 1, "");
	pai_dalloc(tsdn, &shard2->pai, edata2, &deferred_work_generated);

	npurge_size = 0;
	ndefer_purge_calls = 0;
	destroy_test_data(shard1);
	destroy_test_data(shard2);
	base_delete(TSDN_NULL, central_base);
}
TEST_END

TEST_BEGIN(test_central_pool_with_delay) {
	test_skip_if(!hpa_supported() || !config_stats);

	hpa_hooks_t hooks;
	hooks.map = &defer_test_map;
	hooks.unmap = &defer_test_unmap;
	hooks.purge = &defer_test_purge;
	hooks.hugify = &defer_test_hugify;
	hooks.dehugify = &defer_test_dehugify;
	hooks.curtime = &defer_test_curtime;
	hooks.ms_since = &defer_test_ms_since;
	hooks.vectorized_purge = &defer_vectorized_purge;

	hpa_shard_opts_t opts = test_hpa_shard_opts_default;
	opts.deferral_allowed = true;
	opts.purge_threshold = HUGEPAGE;
	opts.min_purge_delay_ms = 1000;
	opts.min_purge_interval_ms = 0;

	hpa_central_t central;
	base_t       *central_base = base_new(TSDN_NULL, /* ind */ 1234,
	          &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);
	assert_ptr_not_null(central_base, "");
	hpa_central_init(&central, central_base, &hooks);
	ndefer_purge_calls = 0;
	hpa_shard_t *shard1 = create_test_data(&central, &opts, SHARD_IND);
	hpa_shard_t *shard2 = create_test_data(&central, &opts, SHARD_IND2);

	bool deferred_work_generated = false;
	nstime_init(&defer_curtime, 10 * 1000 * 1000);
	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	enum { NALLOCS = HUGEPAGE_PAGES };
	edata_t *edatas[NALLOCS];
	for (int i = 0; i < NALLOCS / 2; i++) {
		edatas[i] = pai_alloc(tsdn, &shard1->pai, PAGE, PAGE, false,
		    false, false, &deferred_work_generated);
		expect_ptr_not_null(edatas[i], "Unexpected null edata");
	}
	/* Remember the page */
	hpdata_t *ps = psset_pick_alloc(&shard1->psset, PAGE);
	expect_true(hpdata_huge_get(ps), "Should be huge as we start as huge");

	/* Deallocate all */
	for (int i = 0; i < NALLOCS / 2; i++) {
		pai_dalloc(
		    tsdn, &shard1->pai, edatas[i], &deferred_work_generated);
	}
	hpa_shard_do_deferred_work(tsdn, shard1);
	expect_true(deferred_work_generated, "");
	expect_zu_eq(0, ndefer_purge_calls, "No purge, no donation delay=0ms");

	/* Stats should include the page */
	expect_zu_eq(shard1->psset.stats.merged.nactive, 0, "");
	expect_zu_eq(shard1->psset.stats.merged.npageslabs, 1, "");

	/* One more second passed */
	nstime_iadd(&defer_curtime, UINT64_C(1000) * 1000 * 1000);
	hpa_shard_do_deferred_work(tsdn, shard1);
	expect_zu_eq(0, ndefer_purge_calls, "No purge, donation");
	/* Stats should not include the page */
	expect_zu_eq(shard1->psset.stats.merged.nactive, 0, "");
	expect_zu_eq(shard1->psset.stats.merged.npageslabs, 0, "");
	/* Make allocation on second shard */
	edata_t *edata2 = pai_alloc(tsdn, &shard2->pai, PAGE, PAGE, false,
	    false, false, &deferred_work_generated);
	expect_ptr_not_null(edata2, "Unexpected null edata");
	expect_zu_eq(shard2->psset.stats.merged.nactive, 1, "");
	hpdata_t *ps2 = psset_pick_alloc(&shard2->psset, PAGE);
	expect_ptr_eq(
	    ps, ps2, "Expected to get the same page via central pool");
	expect_true(hpdata_huge_get(ps2), "Should still be huge");
	expect_zu_eq(shard2->psset.stats.merged.npageslabs, 1, "");

	npurge_size = 0;
	ndefer_purge_calls = 0;
	destroy_test_data(shard1);
	destroy_test_data(shard2);
	base_delete(TSDN_NULL, central_base);
}
TEST_END

int
main(void) {
	return test_no_reentrancy(
	    test_central_pool, test_central_pool_with_delay);
}
