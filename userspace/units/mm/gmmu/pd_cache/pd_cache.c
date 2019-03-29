/*
 * Copyright (c) 2018-2019, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <unit/io.h>
#include <unit/core.h>
#include <unit/unit.h>
#include <unit/unit-requirement-ids.h>

#include <nvgpu/gk20a.h>
#include <nvgpu/gmmu.h>
#include <nvgpu/pd_cache.h>
#include <nvgpu/enabled.h>

#include <nvgpu/posix/kmem.h>
#include <nvgpu/posix/posix-fault-injection.h>

#include "common/mm/gmmu/pd_cache_priv.h"

#include "gp10b/mm_gp10b.h"

/*
 * Direct allocs are allocs large enough to just pass straight on to the
 * DMA allocator. Basically that means the size of the PD is larger than a page.
 */
struct pd_cache_alloc_direct_gen {
	u32 bytes;
	u32 nr;
	u32 nr_allocs_before_free;
	u32 nr_frees_before_alloc;
};

/*
 * Direct alloc testing: i.e larger than a page allocs.
 */
static struct pd_cache_alloc_direct_gen alloc_direct_1xPAGE = {
	.bytes = PAGE_SIZE,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024xPAGE = {
	.bytes = PAGE_SIZE,
	.nr    = 1024U,
};
static struct pd_cache_alloc_direct_gen alloc_direct_1x16PAGE = {
	.bytes = 16U * PAGE_SIZE,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024x16PAGE = {
	.bytes = 16U * PAGE_SIZE,
	.nr    = 1024U,
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024xPAGE_x32x24 = {
	.bytes = PAGE_SIZE,
	.nr    = 1024U,
	.nr_allocs_before_free = 32U,
	.nr_frees_before_alloc = 24U
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024xPAGE_x16x4 = {
	.bytes = PAGE_SIZE,
	.nr    = 1024U,
	.nr_allocs_before_free = 16U,
	.nr_frees_before_alloc = 4U
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024xPAGE_x16x15 = {
	.bytes = PAGE_SIZE,
	.nr    = 1024U,
	.nr_allocs_before_free = 16U,
	.nr_frees_before_alloc = 15U
};
static struct pd_cache_alloc_direct_gen alloc_direct_1024xPAGE_x16x1 = {
	.bytes = PAGE_SIZE,
	.nr    = 1024U,
	.nr_allocs_before_free = 16U,
	.nr_frees_before_alloc = 1U
};

/*
 * Sub-page sized allocs. This will test the logic of the pd_caching.
 */
static struct pd_cache_alloc_direct_gen alloc_1x256B = {
	.bytes = 256U,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_1x512B = {
	.bytes = 512U,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_1x1024B = {
	.bytes = 1024U,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_1x2048B = {
	.bytes = 2048U,
	.nr    = 1U,
};
static struct pd_cache_alloc_direct_gen alloc_1024x256B_x16x15 = {
	.bytes = 256U,
	.nr    = 1024U,
	.nr_allocs_before_free = 16U,
	.nr_frees_before_alloc = 15U
};
static struct pd_cache_alloc_direct_gen alloc_1024x256B_x16x1 = {
	.bytes = 256U,
	.nr    = 1024U,
	.nr_allocs_before_free = 16U,
	.nr_frees_before_alloc = 1U
};
static struct pd_cache_alloc_direct_gen alloc_1024x256B_x32x1 = {
	.bytes = 256U,
	.nr    = 1024U,
	.nr_allocs_before_free = 32U,
	.nr_frees_before_alloc = 1U
};
static struct pd_cache_alloc_direct_gen alloc_1024x256B_x11x3 = {
	.bytes = 256U,
	.nr    = 1024U,
	.nr_allocs_before_free = 11U,
	.nr_frees_before_alloc = 3U
};

/*
 * Init a PD cache for us to use.
 */
static int init_pd_cache(struct unit_module *m,
			 struct gk20a *g, struct vm_gk20a *vm)
{
	int err;

	/*
	 * Make sure there's not already a pd_cache inited.
	 */
	if (g->mm.pd_cache != NULL) {
		unit_return_fail(m, "pd_cache already inited\n");
	}

	/*
	 * This is just enough init of the VM to get this code to work. Really
	 * these APIs should just take the gk20a struct...
	 */
	vm->mm = &g->mm;

	err = nvgpu_pd_cache_init(g);
	if (err != 0) {
		unit_return_fail(m, "nvgpu_pd_cache_init failed ??\n");
	}

	return UNIT_SUCCESS;
}

/*
 * Generate a test based on the args in @args. The test is very simple. It
 * allocates nr allocs of the passed size either all at once or in an
 * interleaved pattern.
 *
 * If nr_allocs_before_free is set then this value will determine how many
 * allocs to do before trying frees. If unset it will be simply be nr.
 *
 * If nr_free_before_alloc is set this will determine the number of frees to
 * do before swapping back to allocs. This way you can control the interleaving
 * pattern to some degree. If not set it defaults to nr_allocs_before_free.
 *
 * Anything left over after the last free loop will be freed in one big loop.
 */
static int test_pd_cache_alloc_gen(struct unit_module *m,
				   struct gk20a *g, void *args)
{
	u32 i, j;
	int err;
	int test_status = UNIT_SUCCESS;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd *pds;
	struct pd_cache_alloc_direct_gen *test_spec = args;

	pds = malloc(sizeof(*pds) * test_spec->nr);
	if (pds == NULL) {
		unit_return_fail(m, "OOM in unit test ??\n");
	}

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	if (test_spec->nr_allocs_before_free == 0U) {
		test_spec->nr_allocs_before_free = test_spec->nr;
		test_spec->nr_frees_before_alloc = 0U;
	}

	/*
	 * This takes the test spec and executes some allocs/frees.
	 */
	i = 0U;
	while (i < test_spec->nr) {
		bool do_break = false;

		/*
		 * Do some allocs. Note the i++. Keep marching i along.
		 */
		for (j = 0U; j < test_spec->nr_allocs_before_free; j++) {
			struct nvgpu_gmmu_pd *c = &pds[i++];

			memset(c, 0, sizeof(*c));
			err = nvgpu_pd_alloc(&vm, c, test_spec->bytes);
			if (err != 0) {
				unit_err(m, "%s():%d Failed to do an alloc\n",
					 __func__, __LINE__);
				goto cleanup_err;
			}

			if (i >= test_spec->nr) {
				/* Break the while loop too! */
				do_break = true;
				break;
			}
		}

		/*
		 * And now the frees. The --i is done for the same reason as the
		 * i++ in the alloc loop.
		 */
		for (j = 0U; j < test_spec->nr_frees_before_alloc; j++) {
			struct nvgpu_gmmu_pd *c = &pds[--i];

			/*
			 * Can't easily verify this works directly. Will have to
			 * do that later...
			 */
			nvgpu_pd_free(&vm, c);
		}

		/*
		 * Without this we alloc/free and incr/decr i forever...
		 */
		if (do_break) {
			break;
		}
	}

	/*
	 * We may well have a lot more frees to do!
	 */
	while (i > 0) {
		i--;
		nvgpu_pd_free(&vm, &pds[i]);
	}

	/*
	 * After freeing everything all the pd_cache entries should be cleaned
	 * up. This is not super easy to verify because the pd_cache impl hides
	 * it's data structures within the C code itself.
	 *
	 * We can at least check that the mem field within the nvgpu_gmmu_pd
	 * struct is zeroed. That implies that the nvgpu_pd_free() routine did
	 * at least run through the cleanup code on this nvgpu_gmmu_pd.
	 */
	for (i = 0U; i < test_spec->nr; i++) {
		if (pds[i].mem != NULL) {
			unit_err(m, "%s():%d PD was not freed: %u\n",
				 __func__, __LINE__, i);
			test_status = UNIT_FAIL;
		}
	}

	free(pds);
	nvgpu_pd_cache_fini(g);
	return test_status;

cleanup_err:
	for (i = 0U; i < test_spec->nr; i++) {
		if (pds[i].mem != NULL) {
			nvgpu_pd_free(&vm, &pds[i]);
		}
	}

	free(pds);
	nvgpu_pd_cache_fini(g);

	return UNIT_FAIL;
}

/*
 * Test free on empty PD cache. But make it interesting by doing a valid alloc
 * and freeing that alloc twice. Also verify NULL doesn't cause issues.
 */
static int test_pd_free_empty_pd(struct unit_module *m,
				 struct gk20a *g, void *args)
{
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/* First test cached frees. */
	err = nvgpu_pd_alloc(&vm, &pd, 2048U);
	if (err != 0) {
		unit_return_fail(m, "PD alloc failed");
	}

	/*
	 * nvgpu_pd_free() has no return value so we can't check this directly.
	 * So we will make sure we don't crash.
	 */
	nvgpu_pd_free(&vm, &pd);
	if (!EXPECT_BUG(nvgpu_pd_free(&vm, &pd))) {
		unit_return_fail(m, "nvgpu_pd_free did not BUG() as expected");
	}
	/* When BUG() occurs the pd_cache lock is not released, so do it here */
	nvgpu_mutex_release(&g->mm.pd_cache->lock);

	pd.mem = NULL;
	if (!EXPECT_BUG(nvgpu_pd_free(&vm, &pd))) {
		unit_return_fail(m, "nvgpu_pd_free did not BUG() as expected");
	}
	nvgpu_mutex_release(&g->mm.pd_cache->lock);

	/* And now direct frees. */
	memset(&pd, 0U, sizeof(pd));
	err = nvgpu_pd_alloc(&vm, &pd, PAGE_SIZE);
	if (err != 0) {
		unit_return_fail(m, "PD alloc failed");
	}

	nvgpu_pd_free(&vm, &pd);

	/*
	 * nvgpu_pd_free calls below will not cause BUG() because pd->cached is
	 * true.
	 */
	nvgpu_pd_free(&vm, &pd);

	pd.mem = NULL;
	nvgpu_pd_free(&vm, &pd);

	nvgpu_pd_cache_fini(g);

	return UNIT_SUCCESS;
}

/*
 * Test invalid nvgpu_pd_alloc() calls. Invalid bytes, invalid pd_cache, etc.
 */
static int test_pd_alloc_invalid_input(struct unit_module *m,
				       struct gk20a *g, void *args)
{
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;
	u32 i, garbage[] = { 0U, 128U, 255U, 4095U, 3000U, 128U, 2049U };

	if (g->mm.pd_cache != NULL) {
		unit_return_fail(m, "pd_cache already inited\n");
	}

	/* Obviously shouldn't work pd_cache is not init'ed. */
	if (!EXPECT_BUG(nvgpu_pd_alloc(&vm, &pd, 2048U))) {
		unit_return_fail(m, "pd_alloc worked on NULL pd_cache\n");
	}

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/* Test garbage input. */
	for (i = 0U; i < (sizeof(garbage) / sizeof(garbage[0])); i++) {
		err = nvgpu_pd_alloc(&vm, &pd, garbage[i]);
		if (err == 0) {
			unit_return_fail(m, "PD alloc success: %u (failed)\n",
					 garbage[i]);
		}
	}

	nvgpu_pd_cache_fini(g);

	return UNIT_SUCCESS;
}

static int test_pd_alloc_direct_fi(struct unit_module *m,
				   struct gk20a *g, void *args)
{
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;
	struct nvgpu_posix_fault_inj *kmem_fi =
		nvgpu_kmem_get_fault_injection();
	struct nvgpu_posix_fault_inj *dma_fi =
		nvgpu_dma_alloc_get_fault_injection();

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/*
	 * The alloc_direct() call is easy: there's two places we can fail. One
	 * is allocating the nvgpu_mem struct, the next is the DMA alloc into
	 * the nvgpu_mem struct. Inject faults for these and verify we A) don't
	 * crash and that the allocs are recorded as failures.
	 */

	nvgpu_posix_enable_fault_injection(kmem_fi, true, 0);
	err = nvgpu_pd_alloc(&vm, &pd, PAGE_SIZE);
	if (err == 0) {
		unit_return_fail(m, "pd_alloc() success with kmem OOM\n");
	}
	nvgpu_posix_enable_fault_injection(kmem_fi, false, 0);

	nvgpu_posix_enable_fault_injection(dma_fi, true, 0);
	err = nvgpu_pd_alloc(&vm, &pd, PAGE_SIZE);
	if (err == 0) {
		unit_return_fail(m, "pd_alloc() success with DMA OOM\n");
	}
	nvgpu_posix_enable_fault_injection(dma_fi, false, 0);

	nvgpu_pd_cache_fini(g);
	return UNIT_SUCCESS;
}

static int test_pd_alloc_fi(struct unit_module *m,
			    struct gk20a *g, void *args)
{
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;
	struct nvgpu_posix_fault_inj *kmem_fi =
		nvgpu_kmem_get_fault_injection();
	struct nvgpu_posix_fault_inj *dma_fi =
		nvgpu_dma_alloc_get_fault_injection();

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/*
	 * nvgpu_pd_alloc_new() is effectively the same. We know we will hit the
	 * faults in the new alloc since we have no prior allocs. Therefor we
	 * won't hit a partial alloc and miss the DMA/kmem allocs.
	 */
	nvgpu_posix_enable_fault_injection(kmem_fi, true, 0);
	err = nvgpu_pd_alloc(&vm, &pd, 2048U);
	if (err == 0) {
		unit_return_fail(m, "pd_alloc() success with kmem OOM\n");
	}
	nvgpu_posix_enable_fault_injection(kmem_fi, false, 0);

	nvgpu_posix_enable_fault_injection(dma_fi, true, 0);
	err = nvgpu_pd_alloc(&vm, &pd, 2048U);
	if (err == 0) {
		unit_return_fail(m, "pd_alloc() success with DMA OOM\n");
	}
	nvgpu_posix_enable_fault_injection(dma_fi, false, 0);

	nvgpu_pd_cache_fini(g);
	return UNIT_SUCCESS;
}

/*
 * Test nvgpu_pd_cache_init() - make sure that:
 *
 *   1. Check that init with a memory failure returns -ENOMEM and that the
 *      pd_cache is not initialized.
 *   2. Initial init works.
 *   3. That re-init doesn't re-allocate any resources.
 */
static int test_pd_cache_init(struct unit_module *m,
			      struct gk20a *g, void *args)
{
	int err, i;
	struct nvgpu_pd_cache *cache;
	struct nvgpu_posix_fault_inj *kmem_fi =
		nvgpu_kmem_get_fault_injection();

	/*
	 * Test 1 - do some SW fault injection to make sure we hit the -ENOMEM
	 * potential when initializing the pd cache.
	 */
	nvgpu_posix_enable_fault_injection(kmem_fi, true, 0);
	err = nvgpu_pd_cache_init(g);
	if (err != -ENOMEM) {
		unit_return_fail(m, "OOM condition didn't lead to -ENOMEM\n");
	}

	if (g->mm.pd_cache != NULL) {
		unit_return_fail(m, "PD cache init'ed with no mem\n");
	}
	nvgpu_posix_enable_fault_injection(kmem_fi, false, 0);

	/*
	 * Test 2: Make sure that the init function initializes the necessary
	 * pd_cache data structure within the GPU @g. Just checks some internal
	 * data structures for their presence to make sure this code path has
	 * run.
	 */
	err = nvgpu_pd_cache_init(g);
	if (err != 0) {
		unit_return_fail(m, "PD cache failed to init!\n");
	}

	if (g->mm.pd_cache == NULL) {
		unit_return_fail(m, "PD cache data structure not inited!\n");
	}

	/*
	 * Test 3: make sure that any re-init call doesn't blow away a
	 * previously inited pd_cache.
	 */
	cache = g->mm.pd_cache;
	for (i = 0; i < 5; i++) {
		nvgpu_pd_cache_init(g);
	}

	if (g->mm.pd_cache != cache) {
		unit_return_fail(m, "PD cache got re-inited!\n");
	}

	/*
	 * Leave the PD cache inited at this point...
	 */
	return UNIT_SUCCESS;
}

/*
 * Test nvgpu_pd_cache_fini() - make sure that:
 *
 *   1. An actually allocated cache is cleaned up.
 *   2. If there is no cache this code doesn't crash.
 *
 * Note: this inherits the already inited pd_cache from test_pd_cache_init().
 */
static int test_pd_cache_fini(struct unit_module *m,
			      struct gk20a *g, void *args)
{
	if (g->mm.pd_cache == NULL) {
		unit_return_fail(m, "Missing an init'ed pd_cache\n");
	}

	/*
	 * Test 1: make sure the function pointer is NULL as that implies we
	 * made it to the nvgpu_kfree().
	 */
	nvgpu_pd_cache_fini(g);
	if (g->mm.pd_cache != NULL) {
		unit_return_fail(m, "Failed to cleanup pd_cache\n");
	}

	/*
	 * Test 2: this one is hard to test for functionality - just make sure
	 * we don't crash.
	 */
	nvgpu_pd_cache_fini(g);

	return UNIT_SUCCESS;
}

/**
 * Requirement NVGPU-RQCD-68.C1
 *
 *   Valid/Invalid: The pd_cache does/does not allocate a suitable DMA'able
 *                  buffer of memory.
 *
 * Requirement NVGPU-RQCD-68.C2
 *
 *   Valid/Invalid: The allocated PD is/is not sufficiently aligned for use by
 *                  the GMMU.
 *
 * Requirement NVGPU-RQCD-124.C1
 *
 *   Valid/Invalid: After initialization of the pd_cache the pd_cache can/cannot
 *                  allocate valid PDs.
 */
static int test_pd_cache_valid_alloc(struct unit_module *m,
				     struct gk20a *g, void *args)
{
	u32 bytes;
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/*
	 * Allocate a PD of each valid PD size and ensure they are properly
	 * populated with nvgpu_mem data. This tests read/write and alignment.
	 * This covers the VCs 1 and 2.
	 */
	bytes = 256; /* 256 bytes is the min PD size. */
	while (bytes <= PAGE_SIZE) {

		err = nvgpu_pd_alloc(&vm, &pd, bytes);
		if (err) {
			goto fail;
		}

		/*
		 * Do a write to the zeroth word and then verify this made it to
		 * the nvgpu_mem. Using the zeroth word makes it easy to read
		 * back.
		 */
		nvgpu_pd_write(g, &pd, 0, 0x12345678);

		if (0x12345678 !=
		    nvgpu_mem_rd32(g, pd.mem, pd.mem_offs / sizeof(u32))) {
			nvgpu_pd_free(&vm, &pd);
			goto fail;
		}

		/*
		 * Check alignment is at least as much as the size.
		 */
		if ((pd.mem_offs & (bytes - 1)) != 0) {
			nvgpu_pd_free(&vm, &pd);
			goto fail;
		}

		nvgpu_pd_free(&vm, &pd);

		bytes <<= 1;
	}

	nvgpu_pd_cache_fini(g);
	return UNIT_SUCCESS;

fail:
	nvgpu_pd_cache_fini(g);
	return err;
}

/**
 * Requirement NVGPU-RQCD-68.C3
 *
 *   Valid/Invalid: 16 256B, 8 512B, etc, PDs can/cannot fit into a single
 *                  page sized DMA allocation.
 */
static int do_test_pd_cache_packing_size(struct unit_module *m, struct gk20a *g,
					 struct vm_gk20a *vm, u32 pd_size)
{
	int err;
	u32 i;
	u32 n = PAGE_SIZE / pd_size;
	struct nvgpu_gmmu_pd pds[n], pd;
	struct nvgpu_posix_fault_inj *dma_fi =
		nvgpu_dma_alloc_get_fault_injection();

	unit_info(m, "Alloc %u PDs in page; PD size=%u bytes\n", n, pd_size);

	/*
	 * Only allow one DMA alloc to happen. If before we alloc N PDs we
	 * see an OOM return then we failed to pack sufficient PDs into the
	 * single DMA page.
	 */
	nvgpu_posix_enable_fault_injection(dma_fi, true, 1);

	for (i = 0U; i < n; i++) {
		err = nvgpu_pd_alloc(vm, &pds[i], pd_size);
		if (err) {
			err = UNIT_FAIL;
			goto cleanup;
		}
	}

	/*
	 * Let's just ensure that we trigger the fault on the next alloc.
	 */
	err = nvgpu_pd_alloc(vm, &pd, pd_size);
	if (err) {
		err = UNIT_SUCCESS;
	} else {
		nvgpu_pd_free(vm, &pd);
		err = UNIT_FAIL;
	}

cleanup:
	/*
	 * If there was a failure don't try and free un-allocated PDs.
	 * Effectively a noop if this test passes.
	 */
	n = i;

	for (i = 0; i < n; i++) {
		nvgpu_pd_free(vm, &pds[i]);
	}
	nvgpu_posix_enable_fault_injection(dma_fi, false, 0);
	return err;
}

/**
 * Requirement NVGPU-RQCD-118.C1
 *
 *   Valid/Invalid: Previously allocated PD entries are/are not re-usable.
 */
static int do_test_pd_reusability(struct unit_module *m, struct gk20a *g,
				  struct vm_gk20a *vm, u32 pd_size)
{
	int err = UNIT_SUCCESS;
	u32 i;
	u32 n = PAGE_SIZE / pd_size;
	struct nvgpu_gmmu_pd pds[n];
	struct nvgpu_posix_fault_inj *dma_fi =
		nvgpu_dma_alloc_get_fault_injection();

	nvgpu_posix_enable_fault_injection(dma_fi, true, 1);

	for (i = 0U; i < n; i++) {
		err = nvgpu_pd_alloc(vm, &pds[i], pd_size);
		if (err) {
			err = UNIT_FAIL;
			goto cleanup;
		}
	}

	/* Free all but one PD so that we ensure the page stays cached. */
	for (i = 1U; i < n; i++) {
		nvgpu_pd_free(vm, &pds[i]);
	}

	/* Re-alloc. Will get a -ENOMEM if another page is alloced. */
	for (i = 1U; i < n; i++) {
		err = nvgpu_pd_alloc(vm, &pds[i], pd_size);
		if (err) {
			err = UNIT_FAIL;
			goto cleanup;
		}
	}

cleanup:
	n = i;

	/* Really cleanup. */
	for (i = 0U; i < n; i++) {
		nvgpu_pd_free(vm, &pds[i]);
	}

	nvgpu_posix_enable_fault_injection(dma_fi, false, 0);
	return err;
}

static int test_per_pd_size(struct unit_module *m,
			    struct gk20a *g, void *args)
{
	int err;
	u32 pd_size;
	struct vm_gk20a vm;
	int (*fn)(struct unit_module *m, struct gk20a *g,
		  struct vm_gk20a *vm, u32 pd_size) = args;

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	pd_size = 256U; /* 256 bytes is the min PD size. */
	while (pd_size < PAGE_SIZE) {
		err = fn(m, g, &vm, pd_size);
		if (err) {
			err = UNIT_FAIL;
			goto cleanup;
		}

		pd_size *= 2U;
	}

	err = UNIT_SUCCESS;

cleanup:
	nvgpu_pd_cache_fini(g);
	return err;
}

/*
 * Read back and compare the pattern to the word in the page directory. Return
 * true if they match, false otherwise.
 */
static bool readback_pd_write(struct gk20a *g, struct nvgpu_gmmu_pd *pd,
			      u32 index, u32 pattern)
{
	u32 offset = index + (pd->mem_offs / sizeof(u32));

	return nvgpu_mem_rd32(g, pd->mem, offset) == pattern;
}

/**
 * Requirement NVGPU-RQCD-122.C1
 *
 *   Valid/Invalid: The pd_cache writes/does not write a word of memory in a
 *                  passed PD.
 *
 * Requirement NVGPU-RQCD-126.C1,2
 *
 *   C1: Valid/Invalid: The pd_cache unit does/does not return a valid word
 *                      offset for a 2 word PDE/PTE.
 *   C2: Valid/Invalid: The pd_cache unit does/does not return a valid word
 *                      offset for a 4 word PDE/PTE.
 *
 * This test hits both the pd_write() and the pd_nvgpu_pd_offset_from_index()
 * functions since these are used to validate each other.
 */
static int test_pd_write(struct unit_module *m, struct gk20a *g, void *args)
{
	int err = UNIT_SUCCESS;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd_2w, pd_4w;
	const struct gk20a_mmu_level *mm_levels =
		gp10b_mm_get_mmu_levels(g, SZ_64K);
	u32 i, indexes[] = { 0U, 16U, 255U };

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	/*
	 * Typical size of the last level dual page PD is 4K bytes - 256 entries
	 * at 16 bytes an entry.
	 */
	err = nvgpu_pd_alloc(&vm, &pd_4w, SZ_4K);
	if (err != UNIT_SUCCESS) {
		goto cleanup;
	}

	/*
	 * Most upper level PDs are 512 entries with 8 bytes per entry: again 4K
	 * bytes.
	 */
	err = nvgpu_pd_alloc(&vm, &pd_2w, SZ_4K);
	if (err != UNIT_SUCCESS) {
		goto cleanup;
	}

	/*
	 * Write to PDs at the given index and read back the value from the
	 * underlying nvgpu_mem.
	 */
	for (i = 0U; i < sizeof(indexes) / sizeof(*indexes); i++) {
		u32 offs_2w = nvgpu_pd_offset_from_index(&mm_levels[2],
							 indexes[i]);
		u32 offs_4w = nvgpu_pd_offset_from_index(&mm_levels[3],
							 indexes[i]);

		nvgpu_pd_write(g, &pd_2w, offs_2w, 0xA5A5A5A5);
		nvgpu_pd_write(g, &pd_4w, offs_4w, 0xA5A5A5A5);

		/* Read back. */
		if (!readback_pd_write(g, &pd_2w, offs_2w, 0xA5A5A5A5)) {
			err = UNIT_FAIL;
			goto cleanup;
		}
		if (!readback_pd_write(g, &pd_4w, offs_4w, 0xA5A5A5A5)) {
			err = UNIT_FAIL;
			goto cleanup;
		}
	}

cleanup:
	nvgpu_pd_free(&vm, &pd_2w);
	nvgpu_pd_free(&vm, &pd_4w);
	nvgpu_pd_cache_fini(g);

	return err;
}

/**
 * Requirement NVGPU-RQCD-123.C1
 *
 *   C1: Valid/Invalid: The pd_cache does/does not provide a valid GPU physical
 *                      address for a given PD.
 */
static int test_gpu_address(struct unit_module *m, struct gk20a *g, void *args)
{
	int err;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;
	u64 addr;

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	err = nvgpu_pd_alloc(&vm, &pd, SZ_4K);
	if (err != UNIT_SUCCESS) {
		nvgpu_pd_cache_fini(g);
		return UNIT_FAIL;
	}

	addr = nvgpu_pd_gpu_addr(g, &pd);
	if (addr == 0ULL) {
		unit_return_fail(m, "GPU address of PD is NULL\n");
	}

	nvgpu_pd_free(&vm, &pd);
	nvgpu_pd_cache_fini(g);

	return UNIT_SUCCESS;
}

/**
 * Requirement NVGPU-RQCD-126.C1,2
 *
 *   C1: Valid/Invalid: The pd_cache unit does/does not return a valid word
 *                      offset for a 2 word PDE/PTE.
 *   C2: Valid/Invalid: The pd_cache unit does/does not return a valid word
 *                      offset for a 4 word PDE/PTE.
 */
static int test_offset_computation(struct unit_module *m, struct gk20a *g,
				   void *args)
{
	const struct gk20a_mmu_level *mm_levels =
		gp10b_mm_get_mmu_levels(g, SZ_64K);
	u32 indexes[]    = { 0U, 4U,  16U, 255U };
	u32 offsets_2w[] = { 0U, 8U,  32U, 510U };
	u32 offsets_4w[] = { 0U, 16U, 64U, 1020U };
	bool fail = false;
	u32 i;

	for (i = 0U; i < sizeof(indexes) / sizeof(*indexes); i++) {
		u32 offs_2w = nvgpu_pd_offset_from_index(&mm_levels[2],
							 indexes[i]);
		u32 offs_4w = nvgpu_pd_offset_from_index(&mm_levels[3],
							 indexes[i]);

		if (offs_2w != offsets_2w[i]) {
			unit_err(m, "2w offset comp failed: [%u] %u -> %u\n",
				 i, indexes[i], offs_2w);
			fail = true;
		}
		if (offs_4w != offsets_4w[i]) {
			unit_err(m, "4w offset comp failed: [%u] %u -> %u\n",
				 i, indexes[i], offs_4w);
			fail = true;
		}
	}

	return fail ? UNIT_FAIL : UNIT_SUCCESS;
}

/**
 * Call this to cover a range of requirement tests:
 *
 * NVGPU-RQCD-124.C1
 *   C1: Valid/Invalid: After initialization of pd_cache pd_cache can/cannot
 *                      allocate valid PDs
 *
 * NVGPU-RQCD-155.C1
 *   C1: Valid/Invalid: Re-initialization does not/does cause subsequent kernel
 *                      or DMA allocations.
 *
 * NVGPU-RQCD-125.C1
 *   C1: Valid/Invalid: The pd_cache unit does/does not release all it's
                        allocated memory (kerrnel and DMA).
 *
 * It's redundant, certainly, but that's fine as this test runs fast.
 */
static int test_init_deinit(struct unit_module *m, struct gk20a *g, void *args)
{
	int err, status = UNIT_SUCCESS;
	struct vm_gk20a vm;
	struct nvgpu_gmmu_pd pd;
	struct nvgpu_posix_fault_inj *kmem_fi =
		nvgpu_kmem_get_fault_injection();
	struct nvgpu_posix_fault_inj *dma_fi =
		nvgpu_dma_alloc_get_fault_injection();

	err = init_pd_cache(m, g, &vm);
	if (err != UNIT_SUCCESS) {
		return err;
	}

	err = nvgpu_pd_alloc(&vm, &pd, SZ_4K);
	if (err != UNIT_SUCCESS) {
		nvgpu_pd_cache_fini(g);
		return UNIT_FAIL;
	}

	nvgpu_posix_enable_fault_injection(kmem_fi, true, 0);
	nvgpu_posix_enable_fault_injection(dma_fi, true, 0);

	/*
	 * Block all allocs and check that we don't hit a -ENOMEM. This proves
	 * that we haven't done any extra allocations on subsequent init calls.
	 */
	err = nvgpu_pd_cache_init(g);
	if (err == -ENOMEM) {
		unit_err(m, "Attempted allocation during multi-init\n");
		status = UNIT_FAIL;
	}

	nvgpu_posix_enable_fault_injection(kmem_fi, false, 0);
	nvgpu_posix_enable_fault_injection(dma_fi, false, 0);

	nvgpu_pd_free(&vm, &pd);
	nvgpu_pd_cache_fini(g);

	return status;
}

/*
 * Init the global env - just make sure we don't try and allocate from VIDMEM
 * when doing dma allocs.
 */
static int test_pd_cache_env_init(struct unit_module *m,
				  struct gk20a *g, void *args)
{
	nvgpu_set_enabled(g, NVGPU_MM_UNIFIED_MEMORY, true);

	g->log_mask = 0;
	if (verbose_lvl(m) >= 1) {
		g->log_mask = gpu_dbg_pd_cache;
	}

	return UNIT_SUCCESS;
}

struct unit_module_test pd_cache_tests[] = {
	UNIT_TEST(env_init,				test_pd_cache_env_init, NULL),
	UNIT_TEST(init,					test_pd_cache_init, NULL),
	UNIT_TEST(fini,					test_pd_cache_fini, NULL),

	/*
	 * Requirement verification tests.
	 */
	UNIT_TEST_REQ("NVGPU-RQCD-68.C1,2",  PD_CACHE_REQ1_UID, "V4",
		      valid_alloc,			test_pd_cache_valid_alloc, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-68.C3",    PD_CACHE_REQ1_UID, "V4",
		      pd_packing,			test_per_pd_size, do_test_pd_cache_packing_size),
	UNIT_TEST_REQ("NVGPU-RQCD-118.C1",   PD_CACHE_REQ2_UID, "V3",
		      pd_reusability,			test_per_pd_size, do_test_pd_reusability),
	UNIT_TEST_REQ("NVGPU-RQCD-122.C1",   PD_CACHE_REQ3_UID, "V3",
		      write,				test_pd_write, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-123.C1",   PD_CACHE_REQ4_UID, "V2",
		      gpu_address,			test_gpu_address, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-126.C1,2", PD_CACHE_REQ5_UID, "V1",
		      offset_comp,			test_offset_computation, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-124.C1",   PD_CACHE_REQ6_UID, "V3",
		      init_deinit,			test_init_deinit, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-155.C1",   PD_CACHE_REQ7_UID, "V2",
		      multi_init,			test_init_deinit, NULL),
	UNIT_TEST_REQ("NVGPU-RQCD-125.C1",   PD_CACHE_REQ8_UID, "V2",
		      deinit,				test_init_deinit, NULL),

	/*
	 * Direct allocs.
	 */
	UNIT_TEST(alloc_direct_1xPAGE,			test_pd_cache_alloc_gen, &alloc_direct_1xPAGE),
	UNIT_TEST(alloc_direct_1024xPAGE,		test_pd_cache_alloc_gen, &alloc_direct_1024xPAGE),
	UNIT_TEST(alloc_direct_1x16PAGE,		test_pd_cache_alloc_gen, &alloc_direct_1x16PAGE),
	UNIT_TEST(alloc_direct_1024x16PAGE,		test_pd_cache_alloc_gen, &alloc_direct_1024x16PAGE),
	UNIT_TEST(alloc_direct_1024xPAGE_x32x24,	test_pd_cache_alloc_gen, &alloc_direct_1024xPAGE_x32x24),
	UNIT_TEST(alloc_direct_1024xPAGE_x16x4,		test_pd_cache_alloc_gen, &alloc_direct_1024xPAGE_x16x4),
	UNIT_TEST(alloc_direct_1024xPAGE_x16x15,	test_pd_cache_alloc_gen, &alloc_direct_1024xPAGE_x16x15),
	UNIT_TEST(alloc_direct_1024xPAGE_x16x1,		test_pd_cache_alloc_gen, &alloc_direct_1024xPAGE_x16x1),

	/*
	 * Cached allocs.
	 */
	UNIT_TEST(alloc_1x256B,				test_pd_cache_alloc_gen, &alloc_1x256B),
	UNIT_TEST(alloc_1x512B,				test_pd_cache_alloc_gen, &alloc_1x512B),
	UNIT_TEST(alloc_1x1024B,			test_pd_cache_alloc_gen, &alloc_1x1024B),
	UNIT_TEST(alloc_1x2048B,			test_pd_cache_alloc_gen, &alloc_1x2048B),
	UNIT_TEST(alloc_1024x256B_x16x15,		test_pd_cache_alloc_gen, &alloc_1024x256B_x16x15),
	UNIT_TEST(alloc_1024x256B_x16x1,		test_pd_cache_alloc_gen, &alloc_1024x256B_x16x1),
	UNIT_TEST(alloc_1024x256B_x32x1,		test_pd_cache_alloc_gen, &alloc_1024x256B_x32x1),
	UNIT_TEST(alloc_1024x256B_x11x3,		test_pd_cache_alloc_gen, &alloc_1024x256B_x11x3),

	/*
	 * Error path testing.
	 */
	UNIT_TEST(free_empty,				test_pd_free_empty_pd, NULL),
	UNIT_TEST(invalid_pd_alloc,			test_pd_alloc_invalid_input, NULL),
	UNIT_TEST(alloc_direct_oom,			test_pd_alloc_direct_fi, NULL),
	UNIT_TEST(alloc_oom,				test_pd_alloc_fi, NULL),
};

UNIT_MODULE(pd_cache, pd_cache_tests, UNIT_PRIO_NVGPU_TEST);