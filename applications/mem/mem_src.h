#ifndef APPLICATIONS_MEM_MEM_SRC_H
#define APPLICATIONS_MEM_MEM_SRC_H

#include <rtthread.h>

#if defined(RT_USING_SMALL_MEM)

typedef rt_mem_t memlab_smem_t;

memlab_smem_t memlab_smem_init(const char *name, void *begin_addr, rt_size_t size);
rt_err_t memlab_smem_detach(memlab_smem_t m);
void *memlab_smem_alloc(memlab_smem_t m, rt_size_t size);
void *memlab_smem_realloc(memlab_smem_t m, void *rmem, rt_size_t newsize);
void memlab_smem_free(void *rmem);

rt_bool_t memlab_smem_is_valid_ptr(memlab_smem_t m, const void *ptr);
rt_size_t memlab_smem_block_size(memlab_smem_t m, const void *ptr);
rt_err_t memlab_smem_check(memlab_smem_t m);
void memlab_smem_trace(memlab_smem_t m);

#endif /* defined(RT_USING_SMALL_MEM) */

#endif /* APPLICATIONS_MEM_MEM_SRC_H */
