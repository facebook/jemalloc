#include "test/jemalloc_test.h"

#include "jemalloc/internal/sec.h"

typedef struct test_data_s test_data_t;
struct test_data_s {
	/*
	 * Must be the first member -- we convert back and forth between the
	 * test_data_t and the sec_t;
	 */
	sec_t   sec;
	base_t *base;
};

static void
test_data_init(tsdn_t *tsdn, test_data_t *tdata, const sec_opts_t *opts) {
	tdata->base = base_new(TSDN_NULL, /* ind */ 123,
	    &ehooks_default_extent_hooks, /* metadata_use_hooks */ true);

	bool err = sec_init(tsdn, &tdata->sec, tdata->base, opts);
	assert_false(err, "Unexpected initialization failure");
	if (tdata->sec.opts.nshards > 0) {
		assert_u_ge(tdata->sec.npsizes, 0,
		    "Zero size classes allowed for caching");
	}
}

static void
destroy_test_data(tsdn_t *tsdn, test_data_t *tdata) {
	/* There is no destroy sec to delete the bins ?! */
	base_delete(tsdn, tdata->base);
}

TEST_BEGIN(test_max_nshards_option_zero) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 0;
	opts.max_alloc = PAGE;
	opts.max_bytes = 512 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_t *edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC should be disabled when nshards==0");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_max_alloc_option_too_small) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 512 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_t *edata = sec_alloc(tsdn, &tdata.sec, 3 * PAGE);
	expect_ptr_null(edata, "max_alloc is 2*PAGE, should not alloc 3*PAGE");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_fill) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 4 * PAGE;
	opts.batch_fill_extra = 2;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Fill the cache with two extents */
	sec_stats_t         stats = {0};
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1, edata2;
	edata_size_set(&edata1, PAGE);
	edata_size_set(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata1);
	edata_list_active_append(&allocs, &edata2);
	sec_fill(tsdn, &tdata.sec, PAGE, &allocs, 2);
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 2 * PAGE, "SEC should have what we filled");
	expect_true(edata_list_active_empty(&allocs),
	    "extents should be consumed by sec");

	/* Try to overfill and confirm that max_bytes is respected. */
	stats.bytes = 0;
	edata_t edata5, edata4, edata3;
	edata_size_set(&edata3, PAGE);
	edata_size_set(&edata4, PAGE);
	edata_size_set(&edata5, PAGE);
	edata_list_active_append(&allocs, &edata3);
	edata_list_active_append(&allocs, &edata4);
	edata_list_active_append(&allocs, &edata5);
	sec_fill(tsdn, &tdata.sec, PAGE, &allocs, 3);
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(
	    stats.bytes, opts.max_bytes, "SEC can't have more than max_bytes");
	expect_false(edata_list_active_empty(&allocs), "Not all should fit");
}
TEST_END

TEST_BEGIN(test_sec_alloc) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = 2 * PAGE;
	opts.max_bytes = 4 * PAGE;
	opts.batch_fill_extra = 1;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Alloc from empty cache returns NULL */
	edata_t *edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC is empty");

	/* Place two extents into the sec */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1, edata2;
	edata_size_set(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(edata_list_active_empty(&allocs), "");
	edata_size_set(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(edata_list_active_empty(&allocs), "");

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 2 * PAGE,
	    "After fill bytes should reflect what is in the cache");
	stats.bytes = 0;

	/* Most recently cached extent should be used on alloc */
	edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_eq(edata, &edata2, "edata2 is most recently used");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, PAGE, "One more item left in the cache");
	stats.bytes = 0;

	/* Alloc can still get extents from cache */
	edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_eq(edata, &edata1, "SEC is not empty");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "No more items after last one is popped");

	/* And cache is empty again */
	edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC is empty");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_dalloc) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Return one extent into the cache */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_size_set(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty, we return one pointer to it */
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	/* Return one more extent, so that we are at the limit */
	edata_t edata2;
	edata_size_set(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	/* Sec can take one more as well and we will be exactly at max_bytes */
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, opts.max_bytes, "Size should match deallocs");
	stats.bytes = 0;

	/*
	 * We are at max_bytes.  Now, we dalloc one more pointer and we go above
	 * the limit.  This will force flush to 3/4 of max_bytes and given that
	 * we have max of 2 pages, we will have to flush two. We will not flush
	 * the one given in the input as it is the most recently used.
	 */
	edata_t edata3;
	edata_size_set(&edata3, PAGE);
	edata_list_active_append(&allocs, &edata3);
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should NOT be consumed");
	expect_ptr_ne(
	    edata_list_active_first(&allocs), &edata3, "edata3 is MRU");
	expect_ptr_ne(
	    edata_list_active_last(&allocs), &edata3, "edata3 is MRU");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(PAGE, stats.bytes, "Should have flushed");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_max_bytes_too_low) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = 4 * PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* Return one extent into the cache. Item is too big */
	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_size_set(&edata1, 3 * PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty, we return one pointer to it */
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should not be consumed");
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_flush) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = 4 * PAGE;
	opts.max_bytes = 1024 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	/* We put in 10 one-page extents, and 10 four-page extents */
	edata_list_active_t allocs1;
	edata_list_active_t allocs4;
	edata_list_active_init(&allocs1);
	edata_list_active_init(&allocs4);
	enum { NALLOCS = 10 };
	edata_t edata1[NALLOCS];
	edata_t edata4[NALLOCS];
	for (int i = 0; i < NALLOCS; i++) {
		edata_size_set(&edata1[i], PAGE);
		edata_size_set(&edata4[i], 4 * PAGE);

		edata_list_active_append(&allocs1, &edata1[i]);
		sec_dalloc(tsdn, &tdata.sec, &allocs1);
		edata_list_active_append(&allocs4, &edata4[i]);
		sec_dalloc(tsdn, &tdata.sec, &allocs4);
	}

	sec_stats_t stats = {0};
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(
	    stats.bytes, 10 * 5 * PAGE, "SEC should have what we filled");
	stats.bytes = 0;

	expect_true(edata_list_active_empty(&allocs1), "");
	sec_flush(tsdn, &tdata.sec, &allocs1);
	expect_false(edata_list_active_empty(&allocs1), "");

	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, 0, "SEC should be empty");
	stats.bytes = 0;
	destroy_test_data(tsdn, &tdata);
}
TEST_END

TEST_BEGIN(test_sec_stats) {
	test_data_t tdata;
	sec_opts_t  opts;
	opts.nshards = 1;
	opts.max_alloc = PAGE;
	opts.max_bytes = 2 * PAGE;

	tsdn_t *tsdn = tsd_tsdn(tsd_fetch());
	test_data_init(tsdn, &tdata, &opts);

	edata_list_active_t allocs;
	edata_list_active_init(&allocs);
	edata_t edata1;
	edata_size_set(&edata1, PAGE);
	edata_list_active_append(&allocs, &edata1);

	/* SEC is empty alloc fails. nmisses==1 */
	edata_t *edata = sec_alloc(tsdn, &tdata.sec, PAGE);
	expect_ptr_null(edata, "SEC should be empty");

	/* SEC is empty, we return one pointer to it. ndalloc_noflush=1 */
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	edata_t edata2;
	edata_size_set(&edata2, PAGE);
	edata_list_active_append(&allocs, &edata2);
	/* Sec can take one more, so ndalloc_noflush=2 */
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_true(
	    edata_list_active_empty(&allocs), "extents should be consumed");

	sec_stats_t stats;
	memset(&stats, 0, sizeof(sec_stats_t));
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(stats.bytes, opts.max_bytes, "Size should match deallocs");
	expect_zu_eq(stats.total.ndalloc_noflush, 2, "");
	expect_zu_eq(stats.total.nmisses, 1, "");

	memset(&stats, 0, sizeof(sec_stats_t));

	/*
	 * We are at max_bytes.  Now, we dalloc one more pointer and we go above
	 * the limit.  This will force flush, so ndalloc_flush = 1.
	 */
	edata_t edata3;
	edata_size_set(&edata3, PAGE);
	edata_list_active_append(&allocs, &edata3);
	sec_dalloc(tsdn, &tdata.sec, &allocs);
	expect_false(
	    edata_list_active_empty(&allocs), "extents should NOT be consumed");
	sec_stats_merge(tsdn, &tdata.sec, &stats);
	expect_zu_eq(PAGE, stats.bytes, "Should have flushed");
	expect_zu_eq(stats.total.ndalloc_flush, 1, "");
	memset(&stats, 0, sizeof(sec_stats_t));
	destroy_test_data(tsdn, &tdata);
}
TEST_END

int
main(void) {
	return test(test_max_nshards_option_zero,
	    test_max_alloc_option_too_small, test_sec_fill, test_sec_alloc,
	    test_sec_dalloc, test_max_bytes_too_low, test_sec_flush,
	    test_sec_stats);
}
