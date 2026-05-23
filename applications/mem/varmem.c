#include <stdlib.h>

#include <rtthread.h>
#include <finsh.h>

/*
 * Variable-size memory allocation demo without lfree.
 *
 * This keeps all blocks in one address-ordered doubly linked list. Allocation
 * always starts scanning from heap_ptr, so it is easier to compare with the
 * optimized small-memory implementation that starts from lfree.
 */

#define VARMEM_POOL_SIZE 1024
#define VARMEM_MIN_SIZE  RT_ALIGN(sizeof(rt_uintptr_t), RT_ALIGN_SIZE)

typedef struct
{
    rt_uint8_t used;
    rt_size_t next;
    rt_size_t prev;
} varmem_item_t;

typedef struct
{
    rt_uint8_t *heap_ptr;
    varmem_item_t *heap_end;
    rt_size_t mem_size;
    rt_size_t used_size;
    rt_size_t max_used_size;
    rt_bool_t inited;
} varmem_heap_t;

#define VARMEM_ITEM_SIZE RT_ALIGN(sizeof(varmem_item_t), RT_ALIGN_SIZE)

static rt_uint8_t g_varmem_pool[VARMEM_POOL_SIZE];
static varmem_heap_t g_varmem;

static void varmem_usage(void)
{
    rt_kprintf("varmem init\n");
    rt_kprintf("varmem info\n");
    rt_kprintf("varmem alloc <size>\n");
    rt_kprintf("varmem free <ptr>\n");
    rt_kprintf("varmem trace\n");
    rt_kprintf("varmem demo\n");
    rt_kprintf("varmem help\n");
}

static rt_bool_t varmem_parse_num(const char *text, rt_ubase_t *value)
{
    char *endptr;
    unsigned long parsed;

    if ((text == RT_NULL) || (*text == '\0') || (*text == '-'))
    {
        return RT_FALSE;
    }

    parsed = strtoul(text, &endptr, 0);
    if (*endptr != '\0')
    {
        return RT_FALSE;
    }

    *value = (rt_ubase_t)parsed;
    return RT_TRUE;
}

static rt_bool_t varmem_parse_ptr(const char *text, void **ptr)
{
    rt_ubase_t value;

    if (!varmem_parse_num(text, &value))
    {
        return RT_FALSE;
    }

    *ptr = (void *)(rt_uintptr_t)value;
    return RT_TRUE;
}

static rt_size_t varmem_offset(const varmem_item_t *item)
{
    return (rt_size_t)((const rt_uint8_t *)item - g_varmem.heap_ptr);
}

static rt_size_t varmem_item_user_size(const varmem_item_t *item)
{
    return item->next - varmem_offset(item) - VARMEM_ITEM_SIZE;
}

static varmem_item_t *varmem_next_item(const varmem_item_t *item)
{
    return (varmem_item_t *)&g_varmem.heap_ptr[item->next];
}

static rt_bool_t varmem_ready(void)
{
    if (!g_varmem.inited)
    {
        rt_kprintf("run 'varmem init' first\n");
        return RT_FALSE;
    }

    return RT_TRUE;
}

static rt_err_t varmem_init_heap(void)
{
    rt_uintptr_t begin;
    rt_uintptr_t end;
    rt_size_t mem_size;
    varmem_item_t *first;

    begin = RT_ALIGN((rt_uintptr_t)g_varmem_pool, RT_ALIGN_SIZE);
    end = RT_ALIGN_DOWN((rt_uintptr_t)g_varmem_pool + sizeof(g_varmem_pool),
                        RT_ALIGN_SIZE);

    if ((end <= begin) ||
        ((end - begin) < (2 * VARMEM_ITEM_SIZE + VARMEM_MIN_SIZE)))
    {
        rt_kprintf("varmem init failed\n");
        return -RT_ERROR;
    }

    mem_size = end - begin - 2 * VARMEM_ITEM_SIZE;

    rt_memset(&g_varmem, 0, sizeof(g_varmem));
    rt_memset(g_varmem_pool, 0, sizeof(g_varmem_pool));

    g_varmem.heap_ptr = (rt_uint8_t *)begin;
    g_varmem.mem_size = mem_size;
    g_varmem.inited = RT_TRUE;

    /*
     * Layout after init:
     * [first item header][free user area: mem_size bytes][heap_end header]
     * first->next stores the heap_end header offset from heap_ptr.
     */
    first = (varmem_item_t *)g_varmem.heap_ptr;
    first->used = 0;
    first->next = g_varmem.mem_size + VARMEM_ITEM_SIZE;
    first->prev = 0;

    g_varmem.heap_end = (varmem_item_t *)&g_varmem.heap_ptr[first->next];
    g_varmem.heap_end->used = 1;
    g_varmem.heap_end->next = first->next;
    g_varmem.heap_end->prev = first->next;

    return RT_EOK;
}

static void varmem_update_max(void)
{
    if (g_varmem.max_used_size < g_varmem.used_size)
    {
        g_varmem.max_used_size = g_varmem.used_size;
    }
}

static void varmem_plug_holes(varmem_item_t *item)
{
    varmem_item_t *next;
    varmem_item_t *prev;

    next = varmem_next_item(item);
    if ((item != next) && !next->used && (next != g_varmem.heap_end))
    {
        item->next = next->next;
        varmem_next_item(next)->prev = varmem_offset(item);
    }

    prev = (varmem_item_t *)&g_varmem.heap_ptr[item->prev];
    if ((prev != item) && !prev->used)
    {
        prev->next = item->next;
        varmem_next_item(item)->prev = varmem_offset(prev);
    }
}

static void *varmem_alloc_block(rt_size_t size)
{
    rt_size_t offset;
    rt_size_t split_offset;
    rt_size_t block_size;
    varmem_item_t *item;
    varmem_item_t *split;

    if ((size == 0) || !g_varmem.inited)
    {
        return RT_NULL;
    }

    size = RT_ALIGN(size, RT_ALIGN_SIZE);
    if (size < VARMEM_MIN_SIZE)
    {
        size = VARMEM_MIN_SIZE;
    }

    if (size > g_varmem.mem_size)
    {
        return RT_NULL;
    }

    for (offset = 0;
         offset <= (g_varmem.mem_size - size);
         offset = ((varmem_item_t *)&g_varmem.heap_ptr[offset])->next)
    {
        item = (varmem_item_t *)&g_varmem.heap_ptr[offset];
        block_size = varmem_item_user_size(item);

        if (item->used || (block_size < size))
        {
            continue;
        }

        if (block_size >= (size + VARMEM_ITEM_SIZE + VARMEM_MIN_SIZE))
        {
            split_offset = offset + VARMEM_ITEM_SIZE + size;
            split = (varmem_item_t *)&g_varmem.heap_ptr[split_offset];
            split->used = 0;
            split->next = item->next;
            split->prev = offset;

            item->next = split_offset;
            varmem_next_item(split)->prev = split_offset;
            g_varmem.used_size += size + VARMEM_ITEM_SIZE;
        }
        else
        {
            g_varmem.used_size += item->next - offset;
        }

        item->used = 1;
        varmem_update_max();

        return (rt_uint8_t *)item + VARMEM_ITEM_SIZE;
    }

    return RT_NULL;
}

static rt_err_t varmem_free_block(void *ptr)
{
    varmem_item_t *item;
    rt_size_t offset;

    if ((ptr == RT_NULL) || !g_varmem.inited)
    {
        return -RT_EINVAL;
    }

    item = (varmem_item_t *)((rt_uint8_t *)ptr - VARMEM_ITEM_SIZE);
    if (!item->used)
    {
        return -RT_EINVAL;
    }

    offset = varmem_offset(item);
    item->used = 0;
    g_varmem.used_size -= item->next - offset;

    varmem_plug_holes(item);
    return RT_EOK;
}

static void varmem_trace(void)
{
    varmem_item_t *item;
    rt_size_t offset;
    rt_size_t size;

    rt_kprintf("heap_ptr      : 0x%08lx\n",
               (unsigned long)(rt_uintptr_t)g_varmem.heap_ptr);
    rt_kprintf("heap_end      : 0x%08lx\n",
               (unsigned long)(rt_uintptr_t)g_varmem.heap_end);
    rt_kprintf("total         : %lu\n", (unsigned long)g_varmem.mem_size);
    rt_kprintf("used          : %lu\n", (unsigned long)g_varmem.used_size);
    rt_kprintf("max_used      : %lu\n", (unsigned long)g_varmem.max_used_size);
    rt_kprintf("search policy : first-fit from heap head, no lfree\n");
    rt_kprintf("\n-- block list --\n");

    for (item = (varmem_item_t *)g_varmem.heap_ptr;
         item != g_varmem.heap_end;
         item = varmem_next_item(item))
    {
        offset = varmem_offset(item);
        size = varmem_item_user_size(item);
        rt_kprintf("off=%-4lu hdr=0x%08lx user=0x%08lx size=%-4lu %s next=%lu prev=%lu\n",
                   (unsigned long)offset,
                   (unsigned long)(rt_uintptr_t)item,
                   (unsigned long)(rt_uintptr_t)((rt_uint8_t *)item + VARMEM_ITEM_SIZE),
                   (unsigned long)size,
                   item->used ? "USED" : "FREE",
                   (unsigned long)item->next,
                   (unsigned long)item->prev);
    }
}

static void varmem_demo(void)
{
    void *p1;
    void *p2;
    void *p3;
    void *p4;

    if (varmem_init_heap() != RT_EOK)
    {
        return;
    }

    rt_kprintf("after init:\n");
    varmem_trace();

    p1 = varmem_alloc_block(16);
    p2 = varmem_alloc_block(32);
    p3 = varmem_alloc_block(64);
    p4 = varmem_alloc_block(16);
    rt_kprintf("\nafter alloc: p1=0x%08lx p2=0x%08lx p3=0x%08lx p4=0x%08lx\n",
               (unsigned long)(rt_uintptr_t)p1,
               (unsigned long)(rt_uintptr_t)p2,
               (unsigned long)(rt_uintptr_t)p3,
               (unsigned long)(rt_uintptr_t)p4);
    varmem_trace();

    varmem_free_block(p2);
    rt_kprintf("\nafter free p2, middle hole cannot merge:\n");
    varmem_trace();

    varmem_free_block(p4);
    rt_kprintf("\nafter free p4, tail free block merges forward:\n");
    varmem_trace();

    varmem_free_block(p3);
    rt_kprintf("\nafter free p3, adjacent free blocks merge:\n");
    varmem_trace();
}

static int varmem_cmd(int argc, char **argv)
{
    if (argc < 2)
    {
        varmem_usage();
        return -RT_EINVAL;
    }

    if (rt_strcmp(argv[1], "help") == 0)
    {
        varmem_usage();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "init") == 0)
    {
        rt_err_t ret;

        ret = varmem_init_heap();
        if (ret != RT_EOK)
        {
            return ret;
        }

        rt_kprintf("varmem initialized\n");
        varmem_trace();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "demo") == 0)
    {
        varmem_demo();
        return RT_EOK;
    }

    if (!varmem_ready())
    {
        return -RT_ERROR;
    }

    if (rt_strcmp(argv[1], "info") == 0)
    {
        rt_kprintf("raw_pool      : 0x%08lx - 0x%08lx (%d bytes)\n",
                   (unsigned long)(rt_uintptr_t)g_varmem_pool,
                   (unsigned long)(rt_uintptr_t)(g_varmem_pool + sizeof(g_varmem_pool) - 1),
                   VARMEM_POOL_SIZE);
        rt_kprintf("item_size     : %lu\n", (unsigned long)VARMEM_ITEM_SIZE);
        rt_kprintf("min_user_size : %lu\n", (unsigned long)VARMEM_MIN_SIZE);
        varmem_trace();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "trace") == 0)
    {
        varmem_trace();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "alloc") == 0)
    {
        rt_ubase_t size_value;
        void *ptr;

        if (argc != 3)
        {
            varmem_usage();
            return -RT_EINVAL;
        }

        if (!varmem_parse_num(argv[2], &size_value))
        {
            rt_kprintf("invalid size: %s\n", argv[2]);
            return -RT_EINVAL;
        }

        ptr = varmem_alloc_block((rt_size_t)size_value);
        if (ptr == RT_NULL)
        {
            rt_kprintf("varmem alloc failed for %lu bytes\n",
                       (unsigned long)size_value);
            return -RT_ENOMEM;
        }

        rt_kprintf("varmem alloc ok: ptr=0x%08lx request=%lu\n",
                   (unsigned long)(rt_uintptr_t)ptr,
                   (unsigned long)size_value);
        varmem_trace();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "free") == 0)
    {
        void *ptr;
        rt_err_t ret;

        if (argc != 3)
        {
            varmem_usage();
            return -RT_EINVAL;
        }

        if (!varmem_parse_ptr(argv[2], &ptr))
        {
            rt_kprintf("invalid pointer: %s\n", argv[2]);
            return -RT_EINVAL;
        }

        ret = varmem_free_block(ptr);
        if (ret != RT_EOK)
        {
            rt_kprintf("varmem free failed: ptr=0x%08lx ret=%d\n",
                       (unsigned long)(rt_uintptr_t)ptr,
                       ret);
            return ret;
        }

        rt_kprintf("varmem free ok: ptr=0x%08lx\n",
                   (unsigned long)(rt_uintptr_t)ptr);
        varmem_trace();
        return RT_EOK;
    }

    rt_kprintf("unknown subcommand: %s\n", argv[1]);
    varmem_usage();
    return -RT_EINVAL;
}

MSH_CMD_EXPORT_ALIAS(varmem_cmd, varmem, variable-size memory allocation demo without lfree);
