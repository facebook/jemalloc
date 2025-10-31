#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

bool opt_ccache = false;

void
ccache_tdata_cleanup(tsd_t *tsd) {
	assert(!have_rseq_support);
}

void
ccache_boot(tsdn_t *tsdn, base_t *base) {
	not_reached();
}

ccache_t *
ccache_get(tsdn_t *tsdn, unsigned cpu_id) {
	not_reached();
	return NULL;
}
