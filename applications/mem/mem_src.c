/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2008-7-12      Bernard      the first version
 * 2010-06-09     Bernard      fix the end stub of heap
 *                             fix memory check in rt_realloc function
 * 2010-07-13     Bernard      fix RT_ALIGN issue found by kuronca
 * 2010-10-14     Bernard      fix rt_realloc issue when realloc a NULL pointer.
 * 2017-07-14     armink       fix rt_realloc issue when new size is 0
 * 2018-10-02     Bernard      Add 64bit support
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

#include <rthw.h>
#include <rtthread.h>

#include "mem_src.h"

#if defined(RT_USING_SMALL_MEM)

#define DBG_TAG           "memlab.smem"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#define MEMLAB_MAGIC4(_a, _b, _c, _d) \
    ((rt_uint32_t)((rt_uint32_t)(_a)        | ((rt_uint32_t)(_b) << 8) | \
                   ((rt_uint32_t)(_c) << 16) | ((rt_uint32_t)(_d) << 24)))

#define MEMLAB_MAGIC_ITEM_POOL  MEMLAB_MAGIC4('P', 'O', 'O', 'L')
#define MEMLAB_MAGIC_ITEM_NEXT  MEMLAB_MAGIC4('N', 'E', 'X', 'T')
#define MEMLAB_MAGIC_ITEM_PREV  MEMLAB_MAGIC4('P', 'R', 'E', 'V')
#define MEMLAB_MAGIC_ITEM_NAME  MEMLAB_MAGIC4('N', 'A', 'M', 'E')
#define MEMLAB_MAGIC_HEAP_PTR   MEMLAB_MAGIC4('H', 'P', 'T', 'R')
#define MEMLAB_MAGIC_HEAP_END   MEMLAB_MAGIC4('H', 'E', 'N', 'D')
#define MEMLAB_MAGIC_LFREE      MEMLAB_MAGIC4('L', 'F', 'R', 'E')
#define MEMLAB_MAGIC_MEM_SIZE   MEMLAB_MAGIC4('M', 'S', 'I', 'Z')

struct rt_small_mem_item
{
    rt_uint32_t             magic_pool_ptr;  /**< magic tag for pool_ptr */
    rt_uintptr_t            pool_ptr;         /**< small memory object addr */
    rt_uint32_t             magic_next;      /**< magic tag for next */
    rt_size_t               next;             /**< next free item */
    rt_uint32_t             magic_prev;      /**< magic tag for prev */
    rt_size_t               prev;             /**< prev free item */
#ifdef RT_USING_MEMTRACE
    rt_uint32_t             magic_thread;    /**< magic tag for thread */
#ifdef ARCH_CPU_64BIT
    rt_uint8_t              thread[8];        /**< thread name */
#else
    rt_uint8_t              thread[4];        /**< thread name */
#endif /* ARCH_CPU_64BIT */
#endif /* RT_USING_MEMTRACE */
};

/**
 * Base structure of small memory object
 */
struct rt_small_mem
{
    struct rt_memory            parent;           /**< inherit from rt_memory */
    rt_uint32_t                 magic_heap_ptr;   /**< magic tag for heap_ptr */
    rt_uint8_t                 *heap_ptr;         /**< pointer to the heap */
    rt_uint32_t                 magic_heap_end;   /**< magic tag for heap_end */
    struct rt_small_mem_item   *heap_end;
    rt_uint32_t                 magic_lfree;      /**< magic tag for lfree */
    struct rt_small_mem_item   *lfree;
    rt_uint32_t                 magic_mem_size;   /**< magic tag for mem_size_aligned */
    rt_size_t                   mem_size_aligned; /**< aligned memory size */
};

#define MIN_SIZE               (sizeof(rt_uintptr_t) + sizeof(rt_size_t) + sizeof(rt_size_t))
#define MEM_MASK               ((~(rt_size_t)0) - 1)
#define MEM_USED(_mem)         ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x1)
#define MEM_FREED(_mem)        ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x0)
#define MEM_ISUSED(_mem)       (((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (~MEM_MASK))
#define MEM_POOL(_mem)         ((struct rt_small_mem *)(((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (MEM_MASK)))
#define MEM_SIZE(_heap, _mem)  (((struct rt_small_mem_item *)(_mem))->next - ((rt_uintptr_t)(_mem) - \
                                (rt_uintptr_t)((_heap)->heap_ptr)) - RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE))

#define MIN_SIZE_ALIGNED       RT_ALIGN(MIN_SIZE, RT_ALIGN_SIZE)
#define SIZEOF_STRUCT_MEM      RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE)

rt_inline void memlab_smem_mark_item_magic(struct rt_small_mem_item *mem)
{
    mem->magic_pool_ptr = MEMLAB_MAGIC_ITEM_POOL;
    mem->magic_next = MEMLAB_MAGIC_ITEM_NEXT;
    mem->magic_prev = MEMLAB_MAGIC_ITEM_PREV;
#ifdef RT_USING_MEMTRACE
    mem->magic_thread = MEMLAB_MAGIC_ITEM_NAME;
#endif /* RT_USING_MEMTRACE */
}

rt_inline rt_bool_t memlab_smem_item_magic_ok(const struct rt_small_mem_item *mem)
{
    if ((mem->magic_pool_ptr != MEMLAB_MAGIC_ITEM_POOL) ||
        (mem->magic_next != MEMLAB_MAGIC_ITEM_NEXT) ||
        (mem->magic_prev != MEMLAB_MAGIC_ITEM_PREV))
    {
        return RT_FALSE;
    }
#ifdef RT_USING_MEMTRACE
    if (mem->magic_thread != MEMLAB_MAGIC_ITEM_NAME)
    {
        return RT_FALSE;
    }
#endif /* RT_USING_MEMTRACE */

    return RT_TRUE;
}

rt_inline void memlab_smem_mark_magic(struct rt_small_mem *small_mem)
{
    small_mem->magic_heap_ptr = MEMLAB_MAGIC_HEAP_PTR;
    small_mem->magic_heap_end = MEMLAB_MAGIC_HEAP_END;
    small_mem->magic_lfree = MEMLAB_MAGIC_LFREE;
    small_mem->magic_mem_size = MEMLAB_MAGIC_MEM_SIZE;
}

rt_inline rt_bool_t memlab_smem_magic_ok(const struct rt_small_mem *small_mem)
{
    return (small_mem->magic_heap_ptr == MEMLAB_MAGIC_HEAP_PTR) &&
           (small_mem->magic_heap_end == MEMLAB_MAGIC_HEAP_END) &&
           (small_mem->magic_lfree == MEMLAB_MAGIC_LFREE) &&
           (small_mem->magic_mem_size == MEMLAB_MAGIC_MEM_SIZE);
}

#ifdef RT_USING_MEMTRACE
rt_inline void memlab_smem_setname(struct rt_small_mem_item *mem, const char *name)
{
    int index;

    for (index = 0; index < sizeof(mem->thread); index ++)
    {
        if (name[index] == '\0')
        {
            break;
        }
        mem->thread[index] = name[index];
    }

    for (; index < sizeof(mem->thread); index ++)
    {
        mem->thread[index] = ' ';
    }
}
#endif /* RT_USING_MEMTRACE */

static struct rt_small_mem *memlab_smem_to_small_mem(memlab_smem_t m)
{
    struct rt_small_mem *small_mem;

    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    small_mem = (struct rt_small_mem *)m;
    RT_ASSERT(memlab_smem_magic_ok(small_mem));

    return small_mem;
}

static struct rt_small_mem_item *memlab_smem_find_item(struct rt_small_mem *small_mem, const void *ptr)
{
    struct rt_small_mem_item *mem;

    if ((small_mem == RT_NULL) || (ptr == RT_NULL))
    {
        return RT_NULL;
    }

    for (mem = (struct rt_small_mem_item *)small_mem->heap_ptr;
         mem != small_mem->heap_end;
         mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next])
    {
        if (!memlab_smem_item_magic_ok(mem))
        {
            return RT_NULL;
        }
        if ((const void *)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM) == ptr)
        {
            return mem;
        }
    }

    return RT_NULL;
}

static void memlab_plug_holes(struct rt_small_mem *m, struct rt_small_mem_item *mem)
{
    struct rt_small_mem_item *nmem;
    struct rt_small_mem_item *pmem;

    RT_ASSERT(memlab_smem_magic_ok(m));
    RT_ASSERT(memlab_smem_item_magic_ok(mem));
    RT_ASSERT((rt_uint8_t *)mem >= m->heap_ptr);
    RT_ASSERT((rt_uint8_t *)mem < (rt_uint8_t *)m->heap_end);

    /* plug hole forward */
    nmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next];
    RT_ASSERT(memlab_smem_item_magic_ok(nmem));
    if ((mem != nmem) && !MEM_ISUSED(nmem) &&
        ((rt_uint8_t *)nmem != (rt_uint8_t *)m->heap_end))
    {
        if (m->lfree == nmem)
        {
            m->lfree = mem;
        }

        nmem->pool_ptr = 0;
        mem->next = nmem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[nmem->next])->prev = (rt_uint8_t *)mem - m->heap_ptr;
    }

    /* plug hole backward */
    pmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->prev];
    RT_ASSERT(memlab_smem_item_magic_ok(pmem));
    if ((pmem != mem) && !MEM_ISUSED(pmem))
    {
        if (m->lfree == mem)
        {
            m->lfree = pmem;
        }

        mem->pool_ptr = 0;
        pmem->next = mem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[mem->next])->prev = (rt_uint8_t *)pmem - m->heap_ptr;
    }
}

memlab_smem_t memlab_smem_init(const char *name, void *begin_addr, rt_size_t size)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;
    rt_uintptr_t start_addr;
    rt_uintptr_t begin_align;
    rt_uintptr_t end_align;
    rt_uintptr_t mem_size;

    small_mem = (struct rt_small_mem *)RT_ALIGN((rt_uintptr_t)begin_addr, RT_ALIGN_SIZE);
    start_addr = (rt_uintptr_t)small_mem + sizeof(*small_mem);
    begin_align = RT_ALIGN((rt_uintptr_t)start_addr, RT_ALIGN_SIZE);
    end_align = RT_ALIGN_DOWN((rt_uintptr_t)begin_addr + size, RT_ALIGN_SIZE);

    if ((end_align > (2 * SIZEOF_STRUCT_MEM)) &&
        ((end_align - 2 * SIZEOF_STRUCT_MEM) >= start_addr))
    {
        mem_size = end_align - begin_align - 2 * SIZEOF_STRUCT_MEM;
    }
    else
    {
        rt_kprintf("memlab init failed, begin=0x%08lx end=0x%08lx\n",
                   (unsigned long)(rt_uintptr_t)begin_addr,
                   (unsigned long)((rt_uintptr_t)begin_addr + size));
        return RT_NULL;
    }

    rt_memset(small_mem, 0, sizeof(*small_mem));
    rt_object_init(&(small_mem->parent.parent), RT_Object_Class_Memory, name);
    memlab_smem_mark_magic(small_mem);
    small_mem->parent.algorithm = "small";
    small_mem->parent.address = begin_align;
    small_mem->parent.total = mem_size;
    small_mem->mem_size_aligned = mem_size;
    small_mem->heap_ptr = (rt_uint8_t *)begin_align;

    LOG_D("init heap begin=0x%08lx size=%lu",
          (unsigned long)(rt_uintptr_t)small_mem->heap_ptr,
          (unsigned long)small_mem->mem_size_aligned);

    mem = (struct rt_small_mem_item *)small_mem->heap_ptr;
    memlab_smem_mark_item_magic(mem);
    mem->pool_ptr = MEM_FREED(small_mem);
    mem->next = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    mem->prev = 0;
#ifdef RT_USING_MEMTRACE
    memlab_smem_setname(mem, "INIT");
#endif /* RT_USING_MEMTRACE */

    small_mem->heap_end = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next];
    memlab_smem_mark_item_magic(small_mem->heap_end);
    small_mem->heap_end->pool_ptr = MEM_USED(small_mem);
    small_mem->heap_end->next = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    small_mem->heap_end->prev = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
#ifdef RT_USING_MEMTRACE
    memlab_smem_setname(small_mem->heap_end, "INIT");
#endif /* RT_USING_MEMTRACE */

    small_mem->lfree = (struct rt_small_mem_item *)small_mem->heap_ptr;

    return &small_mem->parent;
}
RTM_EXPORT(memlab_smem_init);

rt_err_t memlab_smem_detach(memlab_smem_t m)
{
    struct rt_small_mem *small_mem;

    small_mem = memlab_smem_to_small_mem(m);
    rt_object_detach(&(small_mem->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(memlab_smem_detach);

void *memlab_smem_alloc(memlab_smem_t m, rt_size_t size)
{
    rt_size_t ptr;
    rt_size_t ptr2;
    struct rt_small_mem_item *mem;
    struct rt_small_mem_item *mem2;
    struct rt_small_mem *small_mem;

    if (size == 0)
    {
        return RT_NULL;
    }

    small_mem = memlab_smem_to_small_mem(m);
    size = RT_ALIGN(size, RT_ALIGN_SIZE);

    if (size < MIN_SIZE_ALIGNED)
    {
        size = MIN_SIZE_ALIGNED;
    }

    if (size > small_mem->mem_size_aligned)
    {
        LOG_D("alloc no memory");
        return RT_NULL;
    }

    for (ptr = (rt_uint8_t *)small_mem->lfree - small_mem->heap_ptr;
         ptr <= small_mem->mem_size_aligned - size;
         ptr = ((struct rt_small_mem_item *)&small_mem->heap_ptr[ptr])->next)
    {
        mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr];
        RT_ASSERT(memlab_smem_item_magic_ok(mem));

        if ((!MEM_ISUSED(mem)) && ((mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size))
        {
            if (mem->next - (ptr + SIZEOF_STRUCT_MEM) >=
                (size + SIZEOF_STRUCT_MEM + MIN_SIZE_ALIGNED))
            {
                ptr2 = ptr + SIZEOF_STRUCT_MEM + size;

                mem2 = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
                memlab_smem_mark_item_magic(mem2);
                mem2->pool_ptr = MEM_FREED(small_mem);
                mem2->next = mem->next;
                mem2->prev = ptr;
#ifdef RT_USING_MEMTRACE
                memlab_smem_setname(mem2, "    ");
#endif /* RT_USING_MEMTRACE */

                mem->next = ptr2;
                if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
                {
                    ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
                }

                small_mem->parent.used += (size + SIZEOF_STRUCT_MEM);
                if (small_mem->parent.max < small_mem->parent.used)
                {
                    small_mem->parent.max = small_mem->parent.used;
                }
            }
            else
            {
                small_mem->parent.used += mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr);
                if (small_mem->parent.max < small_mem->parent.used)
                {
                    small_mem->parent.max = small_mem->parent.used;
                }
            }

            mem->pool_ptr = MEM_USED(small_mem);
#ifdef RT_USING_MEMTRACE
            if (rt_thread_self())
            {
                memlab_smem_setname(mem, rt_thread_self()->parent.name);
            }
            else
            {
                memlab_smem_setname(mem, "NONE");
            }
#endif /* RT_USING_MEMTRACE */

            if (mem == small_mem->lfree)
            {
                while (MEM_ISUSED(small_mem->lfree) && (small_mem->lfree != small_mem->heap_end))
                {
                    small_mem->lfree = (struct rt_small_mem_item *)&small_mem->heap_ptr[small_mem->lfree->next];
                }

                RT_ASSERT((small_mem->lfree == small_mem->heap_end) || (!MEM_ISUSED(small_mem->lfree)));
            }

            RT_ASSERT((rt_uintptr_t)mem + SIZEOF_STRUCT_MEM + size <= (rt_uintptr_t)small_mem->heap_end);
            RT_ASSERT((rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM) % RT_ALIGN_SIZE == 0);
            RT_ASSERT((((rt_uintptr_t)mem) & (RT_ALIGN_SIZE - 1)) == 0);

            LOG_D("allocate memory at 0x%08lx size=%lu",
                  (unsigned long)(rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM),
                  (unsigned long)(mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr)));

            return (rt_uint8_t *)mem + SIZEOF_STRUCT_MEM;
        }
    }

    return RT_NULL;
}
RTM_EXPORT(memlab_smem_alloc);

void *memlab_smem_realloc(memlab_smem_t m, void *rmem, rt_size_t newsize)
{
    rt_size_t size;
    rt_size_t ptr;
    rt_size_t ptr2;
    struct rt_small_mem_item *mem;
    struct rt_small_mem_item *mem2;
    struct rt_small_mem *small_mem;
    void *nmem;

    small_mem = memlab_smem_to_small_mem(m);
    newsize = RT_ALIGN(newsize, RT_ALIGN_SIZE);

    if (newsize > small_mem->mem_size_aligned)
    {
        LOG_D("realloc out of memory");
        return RT_NULL;
    }
    else if (newsize == 0)
    {
        memlab_smem_free(rmem);
        return RT_NULL;
    }

    if (rmem == RT_NULL)
    {
        return memlab_smem_alloc(&small_mem->parent, newsize);
    }

    RT_ASSERT((((rt_uintptr_t)rmem) & (RT_ALIGN_SIZE - 1)) == 0);
    RT_ASSERT((rt_uint8_t *)rmem >= (rt_uint8_t *)small_mem->heap_ptr);
    RT_ASSERT((rt_uint8_t *)rmem < (rt_uint8_t *)small_mem->heap_end);

    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);
    RT_ASSERT(memlab_smem_item_magic_ok(mem));
    ptr = (rt_uint8_t *)mem - small_mem->heap_ptr;
    size = mem->next - ptr - SIZEOF_STRUCT_MEM;

    if (size == newsize)
    {
        return rmem;
    }

    if (newsize + SIZEOF_STRUCT_MEM + MIN_SIZE < size)
    {
        small_mem->parent.used -= (size - newsize);

        ptr2 = ptr + SIZEOF_STRUCT_MEM + newsize;
        mem2 = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
        memlab_smem_mark_item_magic(mem2);
        mem2->pool_ptr = MEM_FREED(small_mem);
        mem2->next = mem->next;
        mem2->prev = ptr;
#ifdef RT_USING_MEMTRACE
        memlab_smem_setname(mem2, "    ");
#endif /* RT_USING_MEMTRACE */
        mem->next = ptr2;
        if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
        {
            ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
        }

        if (mem2 < small_mem->lfree)
        {
            small_mem->lfree = mem2;
        }

        memlab_plug_holes(small_mem, mem2);
        return rmem;
    }

    nmem = memlab_smem_alloc(&small_mem->parent, newsize);
    if (nmem != RT_NULL)
    {
        rt_memcpy(nmem, rmem, size < newsize ? size : newsize);
        memlab_smem_free(rmem);
    }

    return nmem;
}
RTM_EXPORT(memlab_smem_realloc);

void memlab_smem_free(void *rmem)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;

    if (rmem == RT_NULL)
    {
        return;
    }

    RT_ASSERT((((rt_uintptr_t)rmem) & (RT_ALIGN_SIZE - 1)) == 0);

    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);
    RT_ASSERT(memlab_smem_item_magic_ok(mem));
    small_mem = MEM_POOL(mem);
    RT_ASSERT(small_mem != RT_NULL);
    RT_ASSERT(MEM_ISUSED(mem));
    RT_ASSERT(rt_object_get_type(&small_mem->parent.parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&small_mem->parent.parent));
    RT_ASSERT(((rt_uint8_t *)rmem >= (rt_uint8_t *)small_mem->heap_ptr) &&
              ((rt_uint8_t *)rmem < (rt_uint8_t *)small_mem->heap_end));
    RT_ASSERT(MEM_POOL(&small_mem->heap_ptr[mem->next]) == small_mem);

    LOG_D("release memory 0x%08lx size=%lu",
          (unsigned long)(rt_uintptr_t)rmem,
          (unsigned long)(mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr)));

    mem->pool_ptr = MEM_FREED(small_mem);
#ifdef RT_USING_MEMTRACE
    memlab_smem_setname(mem, "    ");
#endif /* RT_USING_MEMTRACE */

    if (mem < small_mem->lfree)
    {
        small_mem->lfree = mem;
    }

    small_mem->parent.used -= (mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr));
    memlab_plug_holes(small_mem, mem);
}
RTM_EXPORT(memlab_smem_free);

rt_bool_t memlab_smem_is_valid_ptr(memlab_smem_t m, const void *ptr)
{
    struct rt_small_mem *small_mem;
    struct rt_small_mem_item *mem;

    if ((m == RT_NULL) || (ptr == RT_NULL))
    {
        return RT_FALSE;
    }

    if ((rt_object_get_type(&m->parent) != RT_Object_Class_Memory) ||
        !rt_object_is_systemobject(&m->parent))
    {
        return RT_FALSE;
    }

    small_mem = (struct rt_small_mem *)m;
    mem = memlab_smem_find_item(small_mem, ptr);
    if (mem == RT_NULL)
    {
        return RT_FALSE;
    }

    return (MEM_ISUSED(mem) && (MEM_POOL(mem) == small_mem)) ? RT_TRUE : RT_FALSE;
}
RTM_EXPORT(memlab_smem_is_valid_ptr);

rt_size_t memlab_smem_block_size(memlab_smem_t m, const void *ptr)
{
    struct rt_small_mem *small_mem;
    struct rt_small_mem_item *mem;

    if (!memlab_smem_is_valid_ptr(m, ptr))
    {
        return 0;
    }

    small_mem = (struct rt_small_mem *)m;
    mem = memlab_smem_find_item(small_mem, ptr);

    return MEM_SIZE(small_mem, mem);
}
RTM_EXPORT(memlab_smem_block_size);

rt_err_t memlab_smem_check(memlab_smem_t m)
{
    rt_base_t level;
    rt_size_t position;
    struct rt_small_mem *small_mem;
    struct rt_small_mem_item *mem;
    struct rt_small_mem_item *next;

    if (m == RT_NULL)
    {
        return -RT_EINVAL;
    }

    if ((rt_object_get_type(&m->parent) != RT_Object_Class_Memory) ||
        !rt_object_is_systemobject(&m->parent))
    {
        return -RT_ERROR;
    }

    small_mem = (struct rt_small_mem *)m;
    mem = RT_NULL;
    position = 0;

    level = rt_hw_interrupt_disable();
    if (!memlab_smem_magic_ok(small_mem))
    {
        goto __exit;
    }
    for (mem = (struct rt_small_mem_item *)small_mem->heap_ptr;
         mem != small_mem->heap_end;
         mem = next)
    {
        position = (rt_uintptr_t)mem - (rt_uintptr_t)small_mem->heap_ptr;

        if (position > small_mem->mem_size_aligned)
        {
            goto __exit;
        }
        if (!memlab_smem_item_magic_ok(mem))
        {
            goto __exit;
        }
        if (MEM_POOL(mem) != small_mem)
        {
            goto __exit;
        }
        if (mem->next <= position)
        {
            goto __exit;
        }
        if (mem->next > small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
        {
            goto __exit;
        }

        next = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next];
        if (!memlab_smem_item_magic_ok(next))
        {
            goto __exit;
        }
        if ((next != small_mem->heap_end) && (next->prev != position))
        {
            goto __exit;
        }
    }
    rt_hw_interrupt_enable(level);

    return RT_EOK;

__exit:
    rt_hw_interrupt_enable(level);

    rt_kprintf("memlab check failed:\n");
    rt_kprintf("   name: %s\n", small_mem->parent.parent.name);
    rt_kprintf("obj_magic: heap_ptr=0x%08lx heap_end=0x%08lx lfree=0x%08lx mem_size=0x%08lx\n",
               (unsigned long)small_mem->magic_heap_ptr,
               (unsigned long)small_mem->magic_heap_end,
               (unsigned long)small_mem->magic_lfree,
               (unsigned long)small_mem->magic_mem_size);
    rt_kprintf("header : 0x%08lx\n", (unsigned long)(rt_uintptr_t)mem);
    rt_kprintf("offset : %lu\n", (unsigned long)position);
    if (mem != RT_NULL)
    {
        rt_kprintf("magic  : pool=0x%08lx next=0x%08lx prev=0x%08lx",
                   (unsigned long)mem->magic_pool_ptr,
                   (unsigned long)mem->magic_next,
                   (unsigned long)mem->magic_prev);
#ifdef RT_USING_MEMTRACE
        rt_kprintf(" name=0x%08lx", (unsigned long)mem->magic_thread);
#endif /* RT_USING_MEMTRACE */
        rt_kprintf("\n");
        rt_kprintf("next   : %lu\n", (unsigned long)mem->next);
        rt_kprintf("prev   : %lu\n", (unsigned long)mem->prev);
        rt_kprintf("pool   : 0x%08lx\n", (unsigned long)mem->pool_ptr);
    }

    return -RT_ERROR;
}
RTM_EXPORT(memlab_smem_check);

void memlab_smem_trace(memlab_smem_t m)
{
    struct rt_small_mem *small_mem;
    struct rt_small_mem_item *mem;
    rt_size_t size;
    const char *state;

    if (m == RT_NULL)
    {
        rt_kprintf("smemlab trace: memory object is null\n");
        return;
    }

    if ((rt_object_get_type(&m->parent) != RT_Object_Class_Memory) ||
        !rt_object_is_systemobject(&m->parent))
    {
        rt_kprintf("smemlab trace: invalid memory object\n");
        return;
    }

    small_mem = (struct rt_small_mem *)m;

    rt_kprintf("\nsmemlab trace:\n");
    rt_kprintf("name       : %s\n", small_mem->parent.parent.name);
    rt_kprintf("algorithm  : %s\n", small_mem->parent.algorithm);
    rt_kprintf("total      : %lu\n", (unsigned long)small_mem->parent.total);
    rt_kprintf("used       : %lu\n", (unsigned long)small_mem->parent.used);
    rt_kprintf("max_used   : %lu\n", (unsigned long)small_mem->parent.max);
    rt_kprintf("heap_ptr   : 0x%08lx\n", (unsigned long)(rt_uintptr_t)small_mem->heap_ptr);
    rt_kprintf("lfree      : 0x%08lx\n", (unsigned long)(rt_uintptr_t)small_mem->lfree);
    rt_kprintf("heap_end   : 0x%08lx\n", (unsigned long)(rt_uintptr_t)small_mem->heap_end);
    rt_kprintf("obj_magic  : HPTR=0x%08lx HEND=0x%08lx LFRE=0x%08lx MSIZ=0x%08lx\n",
               (unsigned long)small_mem->magic_heap_ptr,
               (unsigned long)small_mem->magic_heap_end,
               (unsigned long)small_mem->magic_lfree,
               (unsigned long)small_mem->magic_mem_size);
    rt_kprintf("\n-- block list --\n");

    for (mem = (struct rt_small_mem_item *)small_mem->heap_ptr;
         mem != small_mem->heap_end;
         mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next])
    {
        size = MEM_SIZE(small_mem, mem);
        state = MEM_ISUSED(mem) ? "USED" : "FREE";

        rt_kprintf("hdr=0x%08lx user=0x%08lx size=%-4lu state=%s",
                   (unsigned long)(rt_uintptr_t)mem,
                   (unsigned long)(rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM),
                   (unsigned long)size,
                   state);
        rt_kprintf(" magic=%s", memlab_smem_item_magic_ok(mem) ? "OK" : "BAD");
#ifdef RT_USING_MEMTRACE
        rt_kprintf(" owner=%c%c%c%c",
                   mem->thread[0], mem->thread[1], mem->thread[2], mem->thread[3]);
#endif /* RT_USING_MEMTRACE */
        if (MEM_POOL(mem) != small_mem)
        {
            rt_kprintf(" ***");
        }
        rt_kprintf("\n");
    }
}
RTM_EXPORT(memlab_smem_trace);

#endif /* defined(RT_USING_SMALL_MEM) */
