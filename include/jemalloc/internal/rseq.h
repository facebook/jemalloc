#ifndef JEMALLOC_INTERNAL_RSEQ_H
#define JEMALLOC_INTERNAL_RSEQ_H

#ifdef RSEQ_SIG
#	define JEMALLOC_RSEQ_SIGNATURE RSEQ_SIG
#else
#	define JEMALLOC_RSEQ_SIGNATURE 0x53053053
#endif

#define RSEQ_ASM_INPUTS_COMMON                                                 \
	[cpu] "r"(cpu), [bin] "r"(bin), [rseq_abi] "r"(rseq_abi),              \
	    [bin_head_offset] "i"(offsetof(cache_bin_t, stack_head)),          \
	    [cpuid_offset] "i"(offsetof(rseq_t, cpu_id)),                      \
	    [cs_offset] "i"(offsetof(rseq_t, rseq_cs))

#define RSEQ_ASM_OUTPUTS_COMMON [failed] "=&q"(failed)

#define __RSEQ_ASM_STORE_CS(label)                                             \
	"leaq __rseq_cs_" #label                                               \
	"%=(%%rip), %%rax\n\t"                                                 \
	"movq %%rax, %c[cs_offset](%[rseq_abi])\n\t"

#define RSEQ_ASM_CS_PREPARE(label)                                             \
	".pushsection __rseq_cs, \"aw\", @progbits\n\t"	\
    ".balign 32\n\t"				\
    "__rseq_cs_" #label "%=:\n\t"		\
    ".long 0x0\n\t" /* version */		\
    ".long 0x0\n\t" /* flags */		\
    ".quad 3f\n\t" /* start_ip */		\
    ".quad 5f - 3f\n\t" /* post_commit_offset */	\
    ".quad 1f\n\t" /* abort_ip */		\
    ".popsection\n\t"				\
    ".pushsection __rseq_abort, \"ax\"\n\t"	\
    ".byte 0x0f, 0x1f, 0x05\n\t"		\
    ".long " STRINGIFY(JEMALLOC_RSEQ_SIGNATURE) "\n\t"	\
    "__rseq_abort_" #label "%=:\n\t"		\
    "1:\n\t" /* abort: */			\
    "jmp 2f\n\t"				\
    ".popsection\n\t"				\
    "2:\n\t" /* restart: */			\
    __RSEQ_ASM_STORE_CS(label)

#define RSEQ_ASM_CS_START                                                      \
	"3:\n\t" /* start: */ /* Load the current cpu id in eax. */            \
	"movl %c[cpuid_offset](%[rseq_abi]), %%eax\n\t"                        \
	"cmpl %[cpu], %%eax\n\t" /* Check if cpu read matches the input. */    \
	"setne %[failed]\n\t"    /* Mark failed if cpu mismatch. */            \
	"jne 5f\n\t"             /* Jump to post-commit. */                    \
	"movq %c[bin_head_offset](%[bin]), %%rax\n\t" /* Get stack_head. */    \
	"testq %%rax, %%rax\n\t" /* Check if stack_head is NULL. */            \
	"setz %[failed]\n\t"     /* Mark failed if bin not ready. */           \
	"jz 5f\n\t"              /*Jump to post-commit. */

#define RSEQ_ASM_CS_COMMIT                                                     \
	"4:\n\t" /* commit: bin->stack_head = new_head; */                     \
	"movq %%rax, %c[bin_head_offset](%[bin])\n\t"                          \
	"5:\n\t" /* post-commit */

static inline void *
thread_pointer_get() {
	void *tp;
	__asm__("mov %%fs:0, %0" : "=r"(tp));
	assert(tp != NULL);
	return tp;
}

JEMALLOC_ALWAYS_INLINE unsigned
rseq_cpu_id_start_get(rseq_t *rseq_abi) {
	assert(rseq_abi != NULL);
	return rseq_abi->cpu_id_start;
}

JEMALLOC_ALWAYS_INLINE rseq_t *
rseq_register(rseq_t *rseq_tdatap) {
	rseq_t *registered = NULL;

#ifdef JEMALLOC_RSEQ_GLIBC
	/* GLIBC might have registered the thread with RSEQ. */
	if (__rseq_size != 0) {
		registered = (rseq_t *)((char *)thread_pointer_get()
		    + __rseq_offset);
	}
#endif

#ifdef JEMALLOC_RSEQ_KERNEL
	/* Try to explicitly register the thread with RSEQ. */
	assert(rseq_tdatap != NULL);
	if (registered == NULL
	    && 0
	        == syscall(__NR_rseq, rseq_tdatap, sizeof(*rseq_tdatap), 0,
	            JEMALLOC_RSEQ_SIGNATURE)) {
		registered = rseq_tdatap;
	}
#endif
	return registered;
}

JEMALLOC_ALWAYS_INLINE void
rseq_unregister(rseq_t *rseq_abi, rseq_t *rseq_tdatap) {
	assert(rseq_abi != NULL && rseq_tdatap != NULL);
	/* Unregister the thread with RSEQ only if it was explicitly registered. */
	if (rseq_tdatap == rseq_abi) {
		syscall(__NR_rseq, rseq_tdatap, sizeof(*rseq_tdatap),
		    RSEQ_FLAG_UNREGISTER, JEMALLOC_RSEQ_SIGNATURE);
	}
}

JEMALLOC_ALWAYS_INLINE void *
rseq_alloc(unsigned cpu, cache_bin_t *bin, rseq_t *rseq_abi) {
	void *ret;
	bool  failed;
	__asm__ __volatile__(RSEQ_ASM_CS_PREPARE(alloc) RSEQ_ASM_CS_START
	    /* if head_low_bits == low_bits_empty */
	    "cmpw %%ax, %c[bin_low_bits_empty_offset](%[bin])\n\t"
	    "sete %[failed]\n\t"       /* Mark failed if bin empty. */
	    "je 5f\n\t"                /* Jump to post-commit. */
	    "movq (%%rax), %[ret]\n\t" /* ret = *stack_head; */
	    "addq $8, %%rax\n\t"       /* new_head = stack_head + 1 */
	    RSEQ_ASM_CS_COMMIT
	    : [ret] "=&r"(ret), RSEQ_ASM_OUTPUTS_COMMON
	    : [bin_low_bits_empty_offset] "i"(
	          offsetof(cache_bin_t, low_bits_empty)),
	    RSEQ_ASM_INPUTS_COMMON
	    : "rax", "cc", "memory");
	if (unlikely(failed)) {
		return NULL;
	}
	assert(ret != NULL);
	return ret;
}

JEMALLOC_ALWAYS_INLINE bool
rseq_dalloc(unsigned cpu, cache_bin_t *bin, rseq_t *rseq_abi, void *ptr) {
	bool failed;
	__asm__ __volatile__(RSEQ_ASM_CS_PREPARE(dalloc) RSEQ_ASM_CS_START
	    /* Check if head_low_bits == low_bits_full */
	    "cmpw %%ax, %c[bin_low_bits_full_offset](%[bin])\n\t"
	    "sete %[failed]\n\t"       /* Mark failed if bin full. */
	    "je 5f\n\t"                /* Jump to post-commit. */
	    "subq $8, %%rax\n\t"       /* new_head = stack_head - 1 */
	    "movq %[ptr], (%%rax)\n\t" /* *new_head = ptr; */
	    RSEQ_ASM_CS_COMMIT
	    : RSEQ_ASM_OUTPUTS_COMMON
	    : [bin_low_bits_full_offset] "i"(
	          offsetof(cache_bin_t, low_bits_full)),
	    [ptr] "r"(ptr), RSEQ_ASM_INPUTS_COMMON
	    : "rax", "cc", "memory");
	return failed;
}

#endif /* JEMALLOC_INTERNAL_RSEQ_H */
