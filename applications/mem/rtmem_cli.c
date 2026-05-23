#include <stdlib.h>

#include <rtthread.h>
#include <finsh.h>

#if defined(RT_USING_SMALL_MEM)

extern rt_err_t __rt_smem_detach(rt_smem_t m);
extern void __rt_smem_free(void *rmem);
extern void *__rt_smem_alloc(rt_smem_t m, rt_size_t size);
extern rt_smem_t __rt_smem_init(const char *name, void *begin_addr, rt_size_t size);

#define RTMEMLAB_POOL_SIZE 1024
#define RTMEMLAB_HEX_WIDTH   16

struct rt_small_mem_item
{
    rt_uintptr_t pool_ptr;
    rt_size_t next;
    rt_size_t prev;
};

struct rt_small_mem
{
    struct rt_memory parent;
    rt_uint8_t *heap_ptr;
    struct rt_small_mem_item *heap_end;
    struct rt_small_mem_item *lfree;
    rt_size_t mem_size_aligned;
};

#define MEM_MASK            ((~(rt_size_t)0) - 1)
#define MEM_ISUSED(_mem)    (((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (~MEM_MASK))
#define MEM_POOL(_mem)      ((struct rt_small_mem *)(((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (MEM_MASK)))
#define SIZEOF_STRUCT_MEM   RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE)
#define MEM_SIZE(_heap, _mem) \
    (((struct rt_small_mem_item *)(_mem))->next - ((rt_uintptr_t)(_mem) - \
    (rt_uintptr_t)((_heap)->heap_ptr)) - SIZEOF_STRUCT_MEM)

static rt_uint8_t g_rtmemlab_pool[RTMEMLAB_POOL_SIZE];
static rt_smem_t g_rtmemlab = RT_NULL;
static rt_bool_t g_rtmemlab_inited = RT_FALSE;

static void rtmemlab_log_hex(uint32_t offset, const uint8_t *buf, uint32_t size)
{
    uint16_t i;
    long aligned_offset;
    long end_addr;
    long addr;

    rt_kprintf("0x%08lx: ", offset);
    for (i = 0; i < RTMEMLAB_HEX_WIDTH; i++)
    {
        rt_kprintf("%02d ", i);
    }
    rt_kprintf("\n");

    aligned_offset = (long)offset & ~0x0F;
    end_addr = (long)offset + (long)size;

    for (addr = aligned_offset; addr < end_addr; addr += RTMEMLAB_HEX_WIDTH)
    {
        int j;

        rt_kprintf("0x%08lx: ", addr);

        for (j = 0; j < RTMEMLAB_HEX_WIDTH; ++j)
        {
            long current_addr = addr + j;
            if ((current_addr >= (long)offset) && (current_addr < end_addr))
            {
                rt_kprintf("%02x ", buf[current_addr - (long)offset]);
            }
            else
            {
                rt_kprintf("   ");
            }
        }

        rt_kprintf(" ");

        for (j = 0; j < RTMEMLAB_HEX_WIDTH; ++j)
        {
            long current_addr = addr + j;
            if ((current_addr >= (long)offset) && (current_addr < end_addr))
            {
                uint8_t c = buf[current_addr - (long)offset];
                rt_kprintf("%c", (c >= 32 && c <= 126) ? c : '.');
            }
            else
            {
                rt_kprintf(".");
            }
        }

        rt_kprintf("\n");
    }
}

static void rtmemlab_usage(void)
{
    rt_kprintf("rtmemlab init\n");
    rt_kprintf("rtmemlab info\n");
    rt_kprintf("rtmemlab alloc <size>\n");
    rt_kprintf("rtmemlab free <ptr>\n");
    rt_kprintf("rtmemlab dump pool [offset] [len]\n");
    rt_kprintf("rtmemlab dump ptr <ptr> <len>\n");
    rt_kprintf("rtmemlab trace\n");
    rt_kprintf("rtmemlab demo\n");
    rt_kprintf("rtmemlab help\n");
    rt_kprintf("example:\n");
    rt_kprintf("rtmemlab init\n");
    rt_kprintf("rtmemlab alloc 16\n");
    rt_kprintf("rtmemlab alloc 32\n");
    rt_kprintf("rtmemlab free 0xXXXXXXXX\n");
    rt_kprintf("rtmemlab dump pool 0 64\n");
}

static rt_bool_t rtmemlab_parse_num(const char *text, rt_ubase_t *value)
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

static rt_bool_t rtmemlab_parse_ptr(const char *text, void **ptr)
{
    rt_ubase_t value;

    if (!rtmemlab_parse_num(text, &value))
    {
        return RT_FALSE;
    }

    *ptr = (void *)(rt_uintptr_t)value;
    return RT_TRUE;
}

static struct rt_small_mem *rtmemlab_to_small_mem(rt_smem_t m)
{
    if (m == RT_NULL)
    {
        return RT_NULL;
    }

    if ((rt_object_get_type(&m->parent) != RT_Object_Class_Memory) ||
        !rt_object_is_systemobject(&m->parent))
    {
        return RT_NULL;
    }

    return (struct rt_small_mem *)m;
}

static rt_bool_t rtmemlab_is_pool_addr(const void *ptr)
{
    const rt_uint8_t *addr;
    const rt_uint8_t *pool_begin;
    const rt_uint8_t *pool_end;

    if (ptr == RT_NULL)
    {
        return RT_FALSE;
    }

    addr = (const rt_uint8_t *)ptr;
    pool_begin = g_rtmemlab_pool;
    pool_end = g_rtmemlab_pool + sizeof(g_rtmemlab_pool);

    return (addr >= pool_begin) && (addr < pool_end);
}

static rt_bool_t rtmemlab_range_in_pool(const void *ptr, rt_size_t len)
{
    const rt_uint8_t *addr;
    const rt_uint8_t *pool_end;

    if (!rtmemlab_is_pool_addr(ptr))
    {
        return RT_FALSE;
    }

    addr = (const rt_uint8_t *)ptr;
    pool_end = g_rtmemlab_pool + sizeof(g_rtmemlab_pool);

    return len <= (rt_size_t)(pool_end - addr);
}

static rt_bool_t rtmemlab_ready(void)
{
    if (!g_rtmemlab_inited || (g_rtmemlab == RT_NULL))
    {
        rt_kprintf("run 'rtmemlab init' first\n");
        return RT_FALSE;
    }

    return RT_TRUE;
}

static void rtmemlab_trace(const char *title)
{
    struct rt_small_mem *heap;
    struct rt_small_mem_item *mem;
    struct rt_small_mem_item *next;
    rt_size_t pos;
    rt_size_t size;
    int index;
    const char *state;

    heap = rtmemlab_to_small_mem(g_rtmemlab);
    if (heap == RT_NULL)
    {
        rt_kprintf("rtmemlab trace: invalid heap object\n");
        return;
    }

    if (title != RT_NULL)
    {
        rt_kprintf("\nrtmemlab trace (%s):\n", title);
    }
    else
    {
        rt_kprintf("\nrtmemlab trace:\n");
    }

    rt_kprintf("name      : %s\n", heap->parent.parent.name);
    rt_kprintf("algorithm : %s\n", heap->parent.algorithm);
    rt_kprintf("total     : %lu\n", (unsigned long)heap->parent.total);
    rt_kprintf("used      : %lu\n", (unsigned long)heap->parent.used);
    rt_kprintf("max_used  : %lu\n", (unsigned long)heap->parent.max);
    rt_kprintf("heap_ptr  : 0x%08lx\n", (unsigned long)(rt_uintptr_t)heap->heap_ptr);
    rt_kprintf("lfree     : 0x%08lx\n", (unsigned long)(rt_uintptr_t)heap->lfree);
    rt_kprintf("heap_end  : 0x%08lx\n", (unsigned long)(rt_uintptr_t)heap->heap_end);
    rt_kprintf("-- block list --\n");

    mem = (struct rt_small_mem_item *)heap->heap_ptr;
    for (index = 0; (mem != heap->heap_end) && (index < 128); index++)
    {
        pos = (rt_size_t)((rt_uint8_t *)mem - heap->heap_ptr);
        if ((mem->next <= pos) || (mem->next > heap->mem_size_aligned + SIZEOF_STRUCT_MEM))
        {
            rt_kprintf("[%02d] off=%lu INVALID next=%lu, stop\n",
                       index, (unsigned long)pos, (unsigned long)mem->next);
            break;
        }

        size = MEM_SIZE(heap, mem);
        state = MEM_ISUSED(mem) ? "USED" : "FREE";

        rt_kprintf("[%02d] off=%-4lu hdr=0x%08lx user=0x%08lx size=%-4lu %s next=%lu prev=%lu\n",
                   index,
                   (unsigned long)pos,
                   (unsigned long)(rt_uintptr_t)mem,
                   (unsigned long)(rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM),
                   (unsigned long)size,
                   state,
                   (unsigned long)mem->next,
                   (unsigned long)mem->prev);

        next = (struct rt_small_mem_item *)&heap->heap_ptr[mem->next];
        mem = next;
    }
}

static struct rt_small_mem_item *rtmemlab_find_active_item(void *ptr)
{
    struct rt_small_mem *heap;
    struct rt_small_mem_item *mem;
    struct rt_small_mem_item *next;
    rt_size_t pos;

    heap = rtmemlab_to_small_mem(g_rtmemlab);
    if ((heap == RT_NULL) || (ptr == RT_NULL))
    {
        return RT_NULL;
    }

    mem = (struct rt_small_mem_item *)heap->heap_ptr;
    while (mem != heap->heap_end)
    {
        pos = (rt_size_t)((rt_uint8_t *)mem - heap->heap_ptr);
        if ((mem->next <= pos) || (mem->next > heap->mem_size_aligned + SIZEOF_STRUCT_MEM))
        {
            return RT_NULL;
        }

        if (((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM == (rt_uint8_t *)ptr) &&
            MEM_ISUSED(mem) &&
            (MEM_POOL(mem) == heap))
        {
            return mem;
        }

        next = (struct rt_small_mem_item *)&heap->heap_ptr[mem->next];
        mem = next;
    }

    return RT_NULL;
}

static int rtmemlab_parse_active_ptr(const char *ptr_text, void **ptr, rt_size_t *block_size)
{
    void *parsed_ptr;
    struct rt_small_mem_item *item;
    struct rt_small_mem *heap;
    rt_size_t size;

    if (!rtmemlab_parse_ptr(ptr_text, &parsed_ptr))
    {
        rt_kprintf("invalid pointer: %s\n", ptr_text);
        return -RT_EINVAL;
    }

    item = rtmemlab_find_active_item(parsed_ptr);
    if (item == RT_NULL)
    {
        rt_kprintf("pointer 0x%08lx is not an active allocation\n",
                   (unsigned long)(rt_uintptr_t)parsed_ptr);
        return -RT_EINVAL;
    }

    heap = rtmemlab_to_small_mem(g_rtmemlab);
    if (heap == RT_NULL)
    {
        rt_kprintf("rtmemlab dump: invalid heap object\n");
        return -RT_ERROR;
    }

    size = MEM_SIZE(heap, item);
    if (ptr != RT_NULL)
    {
        *ptr = parsed_ptr;
    }
    if (block_size != RT_NULL)
    {
        *block_size = size;
    }

    return RT_EOK;
}

static void rtmemlab_hexdump(const void *buf, rt_size_t len)
{
    if ((buf == RT_NULL) || (len == 0))
    {
        return;
    }

    rtmemlab_log_hex((uint32_t)(rt_uintptr_t)buf, (const uint8_t *)buf, (uint32_t)len);
}

static int rtmemlab_cmd_init(void)
{
    if (g_rtmemlab_inited && (g_rtmemlab != RT_NULL))
    {
        __rt_smem_detach(g_rtmemlab);
        g_rtmemlab = RT_NULL;
        g_rtmemlab_inited = RT_FALSE;
    }

    rt_memset(g_rtmemlab_pool, 0, sizeof(g_rtmemlab_pool));

    g_rtmemlab = __rt_smem_init("rtmemlab", g_rtmemlab_pool, sizeof(g_rtmemlab_pool));
    if (g_rtmemlab == RT_NULL)
    {
        rt_kprintf("rtmemlab init failed\n");
        return -RT_ERROR;
    }

    g_rtmemlab_inited = RT_TRUE;
    rt_kprintf("rtmemlab initialized: raw_pool=0x%08lx-0x%08lx (%lu bytes)\n",
               (unsigned long)(rt_uintptr_t)g_rtmemlab_pool,
               (unsigned long)(rt_uintptr_t)(g_rtmemlab_pool + sizeof(g_rtmemlab_pool) - 1),
               (unsigned long)sizeof(g_rtmemlab_pool));
    rtmemlab_trace("after init");
    return RT_EOK;
}

static int rtmemlab_cmd_info(void)
{
    rt_kprintf("raw_pool  : 0x%08lx - 0x%08lx (%lu bytes)\n",
               (unsigned long)(rt_uintptr_t)g_rtmemlab_pool,
               (unsigned long)(rt_uintptr_t)(g_rtmemlab_pool + sizeof(g_rtmemlab_pool) - 1),
               (unsigned long)sizeof(g_rtmemlab_pool));
    rtmemlab_trace("info");
    return RT_EOK;
}

static int rtmemlab_cmd_alloc(const char *size_text)
{
    rt_ubase_t req;
    void *ptr;

    if (!rtmemlab_parse_num(size_text, &req))
    {
        rt_kprintf("invalid size: %s\n", size_text);
        return -RT_EINVAL;
    }

    ptr = __rt_smem_alloc(g_rtmemlab, (rt_size_t)req);
    if (ptr == RT_NULL)
    {
        rt_kprintf("rtmemlab alloc failed for %lu bytes\n", (unsigned long)req);
        rtmemlab_trace("alloc failed");
        return -RT_ENOMEM;
    }

    rt_kprintf("rtmemlab alloc ok: ptr=0x%08lx req=%lu\n",
               (unsigned long)(rt_uintptr_t)ptr,
               (unsigned long)req);
    rtmemlab_trace("after alloc");
    return RT_EOK;
}

static int rtmemlab_cmd_free(const char *ptr_text)
{
    void *ptr;
    rt_size_t block_size;
    int ret;

    ret = rtmemlab_parse_active_ptr(ptr_text, &ptr, &block_size);
    if (ret != RT_EOK)
    {
        rtmemlab_trace("free rejected");
        return ret;
    }

    rt_kprintf("rtmemlab free: ptr=0x%08lx size=%lu\n",
               (unsigned long)(rt_uintptr_t)ptr,
               (unsigned long)block_size);
    __rt_smem_free(ptr);
    rtmemlab_trace("after free");
    return RT_EOK;
}

static int rtmemlab_cmd_dump_pool(int argc, char **argv)
{
    rt_ubase_t offset_value;
    rt_ubase_t len_value;
    rt_size_t actual_len;
    rt_uint8_t *dump_ptr;

    offset_value = 0;
    len_value = sizeof(g_rtmemlab_pool);

    if (argc >= 4)
    {
        if (!rtmemlab_parse_num(argv[3], &offset_value))
        {
            rt_kprintf("invalid offset: %s\n", argv[3]);
            return -RT_EINVAL;
        }
    }

    if (argc >= 5)
    {
        if (!rtmemlab_parse_num(argv[4], &len_value))
        {
            rt_kprintf("invalid len: %s\n", argv[4]);
            return -RT_EINVAL;
        }
    }

    if (offset_value > sizeof(g_rtmemlab_pool))
    {
        rt_kprintf("offset %lu exceeds raw pool size %lu\n",
                   (unsigned long)offset_value,
                   (unsigned long)sizeof(g_rtmemlab_pool));
        return -RT_EINVAL;
    }

    actual_len = (rt_size_t)len_value;
    if (actual_len > (sizeof(g_rtmemlab_pool) - (rt_size_t)offset_value))
    {
        actual_len = sizeof(g_rtmemlab_pool) - (rt_size_t)offset_value;
        rt_kprintf("dump len clipped to %lu bytes\n", (unsigned long)actual_len);
    }

    dump_ptr = g_rtmemlab_pool + offset_value;
    rt_kprintf("dump pool: raw_base=0x%08lx addr=0x%08lx offset=%lu len=%lu\n",
               (unsigned long)(rt_uintptr_t)g_rtmemlab_pool,
               (unsigned long)(rt_uintptr_t)dump_ptr,
               (unsigned long)offset_value,
               (unsigned long)actual_len);

    if (actual_len == 0)
    {
        rt_kprintf("no bytes to dump\n");
        return RT_EOK;
    }

    rtmemlab_hexdump(dump_ptr, actual_len);
    return RT_EOK;
}

static int rtmemlab_cmd_dump_ptr(const char *ptr_text, const char *len_text)
{
    void *ptr;
    rt_size_t block_size;
    rt_ubase_t len_value;
    rt_size_t actual_len;
    int ret;

    ret = rtmemlab_parse_active_ptr(ptr_text, &ptr, &block_size);
    if (ret != RT_EOK)
    {
        return ret;
    }

    if (!rtmemlab_parse_num(len_text, &len_value))
    {
        rt_kprintf("invalid len: %s\n", len_text);
        return -RT_EINVAL;
    }

    actual_len = (rt_size_t)len_value;
    if (actual_len > block_size)
    {
        actual_len = block_size;
        rt_kprintf("dump len clipped to block size %lu\n", (unsigned long)actual_len);
    }

    if (!rtmemlab_range_in_pool(ptr, actual_len))
    {
        rt_kprintf("dump range exceeds raw pool\n");
        return -RT_EINVAL;
    }

    rt_kprintf("dump ptr: addr=0x%08lx len=%lu block=%lu\n",
               (unsigned long)(rt_uintptr_t)ptr,
               (unsigned long)actual_len,
               (unsigned long)block_size);

    if (actual_len == 0)
    {
        rt_kprintf("no bytes to dump\n");
        return RT_EOK;
    }

    rtmemlab_hexdump(ptr, actual_len);
    return RT_EOK;
}

static int rtmemlab_cmd_dump(int argc, char **argv)
{
    if (argc < 4)
    {
        if ((argc == 3) && (rt_strcmp(argv[2], "pool") == 0))
        {
            return rtmemlab_cmd_dump_pool(argc, argv);
        }

        rtmemlab_usage();
        return -RT_EINVAL;
    }

    if (rt_strcmp(argv[2], "pool") == 0)
    {
        if (argc > 5)
        {
            rtmemlab_usage();
            return -RT_EINVAL;
        }

        return rtmemlab_cmd_dump_pool(argc, argv);
    }

    if (rt_strcmp(argv[2], "ptr") == 0)
    {
        if (argc != 5)
        {
            rtmemlab_usage();
            return -RT_EINVAL;
        }

        return rtmemlab_cmd_dump_ptr(argv[3], argv[4]);
    }

    rt_kprintf("unknown dump target: %s\n", argv[2]);
    return -RT_EINVAL;
}

static int rtmemlab_cmd_demo(void)
{
    void *p1;
    void *p2;
    void *p3;
    void *p4;

    if (rtmemlab_cmd_init() != RT_EOK)
    {
        return -RT_ERROR;
    }

    p1 = __rt_smem_alloc(g_rtmemlab, 16);
    rt_kprintf("demo alloc p1=0x%08lx (16)\n", (unsigned long)(rt_uintptr_t)p1);
    rtmemlab_trace("demo alloc p1");

    p2 = __rt_smem_alloc(g_rtmemlab, 32);
    rt_kprintf("demo alloc p2=0x%08lx (32)\n", (unsigned long)(rt_uintptr_t)p2);
    rtmemlab_trace("demo alloc p2");

    p3 = __rt_smem_alloc(g_rtmemlab, 64);
    rt_kprintf("demo alloc p3=0x%08lx (64)\n", (unsigned long)(rt_uintptr_t)p3);
    rtmemlab_trace("demo alloc p3");

    p4 = __rt_smem_alloc(g_rtmemlab, 16);
    rt_kprintf("demo alloc p4=0x%08lx (16)\n", (unsigned long)(rt_uintptr_t)p4);
    rtmemlab_trace("demo alloc p4");

    if (p2 != RT_NULL)
    {
        __rt_smem_free(p2);
        rt_kprintf("demo free p2\n");
        rtmemlab_trace("demo free p2");
    }

    if (p4 != RT_NULL)
    {
        __rt_smem_free(p4);
        rt_kprintf("demo free p4\n");
        rtmemlab_trace("demo free p4");
    }

    if (p3 != RT_NULL)
    {
        __rt_smem_free(p3);
        rt_kprintf("demo free p3\n");
        rtmemlab_trace("demo free p3");
    }

    return RT_EOK;
}

static int rtmemlab_cmd(int argc, char **argv)
{
    if (argc < 2)
    {
        rtmemlab_usage();
        return -RT_EINVAL;
    }

    if (rt_strcmp(argv[1], "help") == 0)
    {
        rtmemlab_usage();
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "init") == 0)
    {
        return rtmemlab_cmd_init();
    }

    if (rt_strcmp(argv[1], "demo") == 0)
    {
        return rtmemlab_cmd_demo();
    }

    if (!rtmemlab_ready())
    {
        return -RT_ERROR;
    }

    if (rt_strcmp(argv[1], "info") == 0)
    {
        return rtmemlab_cmd_info();
    }

    if (rt_strcmp(argv[1], "trace") == 0)
    {
        rtmemlab_trace("manual");
        return RT_EOK;
    }

    if (rt_strcmp(argv[1], "alloc") == 0)
    {
        if (argc != 3)
        {
            rtmemlab_usage();
            return -RT_EINVAL;
        }
        return rtmemlab_cmd_alloc(argv[2]);
    }

    if (rt_strcmp(argv[1], "free") == 0)
    {
        if (argc != 3)
        {
            rtmemlab_usage();
            return -RT_EINVAL;
        }
        return rtmemlab_cmd_free(argv[2]);
    }

    if (rt_strcmp(argv[1], "dump") == 0)
    {
        return rtmemlab_cmd_dump(argc, argv);
    }

    rt_kprintf("unknown subcommand: %s\n", argv[1]);
    rtmemlab_usage();
    return -RT_EINVAL;
}

MSH_CMD_EXPORT_ALIAS(rtmemlab_cmd, rtmemlab, rt small memory demo with auto trace);

#endif /* RT_USING_SMALL_MEM */
