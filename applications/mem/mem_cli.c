// #include <stdlib.h>

// #include <rtthread.h>
// #include <finsh.h>

// #define DBG_TAG           "smemlab"
// #define DBG_LVL           DBG_INFO
// #include <rtdbg.h>

// #include "mem_src.h"

// #if defined(RT_USING_SMALL_MEM)

// #define MEMLAB_POOL_SIZE 1024
// #define MEMLAB_HEX_WIDTH   16

// static rt_uint8_t g_smemlab_pool[MEMLAB_POOL_SIZE];
// static memlab_smem_t g_smemlab = RT_NULL;
// static rt_bool_t g_smemlab_inited = RT_FALSE;

// void _LOG_HEX(uint32_t offset, uint8_t* buf, uint32_t size)
// {
//     // rt_kprintf( "%*s", 12, "" );
//     rt_kprintf("0x%08lx: ", offset);
//     for (uint16_t i = 0; i < 16; i++) {
//         rt_kprintf("%02d ", i);
//     }
//     rt_kprintf("\n");
//     // 计算对齐后的起始地址
//     long aligned_offset = offset & ~0x0F;
//     // 计算数据结束地址
//     long end_addr = offset + size;

//     // 遍历每个对齐的块
//     for (long addr = aligned_offset; addr < end_addr; addr += 16) {
//         // 打印当前块的地址
//         rt_kprintf("0x%08lx: ", addr);

//         // 处理十六进制部分
//         for (int i = 0; i < 16; ++i) {
//             long current_addr = addr + i;
//             if (current_addr >= offset && current_addr < end_addr) {
//                 // 在有效范围内，打印字节
//                 rt_kprintf("%02x ", buf[current_addr - offset]);
//             }
//             else {
//                 // 超出范围，用空格填充
//                 rt_kprintf("   ");
//             }
//         }
//         // 分隔符
//         rt_kprintf(" ");

//         // 处理ASCII部分
//         for (int i = 0; i < 16; ++i) {
//             long current_addr = addr + i;
//             if (current_addr >= offset && current_addr < end_addr) {
//                 uint8_t c = buf[current_addr - offset];
//                 // 可打印字符直接输出，否则用.代替
//                 // putchar(isprint(c) ? c : '.');
//                 rt_kprintf("%c", (c >= 32 && c <= 126) ? c : '.');
//             }
//             else {
//                 // 超出范围的部分用.填充
//                 rt_kprintf(".");
//             }
//         }
//         rt_kprintf("\n");
//     }
// }
// static void smemlab_usage(void)
// {
//     rt_kprintf("smemlab init\n");
//     rt_kprintf("smemlab info\n");
//     rt_kprintf("smemlab alloc <size>\n");
//     rt_kprintf("smemlab realloc <ptr> <size>\n");
//     rt_kprintf("smemlab free <ptr>\n");
//     rt_kprintf("smemlab fill <ptr> <len> <byte>\n");
//     rt_kprintf("smemlab dump pool [offset] [len]\n");
//     rt_kprintf("smemlab dump ptr <ptr> <len>\n");
//     rt_kprintf("smemlab trace\n");
//     rt_kprintf("smemlab check\n");
//     rt_kprintf("smemlab help\n");
// }

// static rt_bool_t smemlab_ready(void)
// {
//     if (!g_smemlab_inited || (g_smemlab == RT_NULL))
//     {
//         rt_kprintf("run 'smemlab init' first\n");
//         return RT_FALSE;
//     }

//     return RT_TRUE;
// }

// static rt_bool_t smemlab_parse_num(const char *text, rt_ubase_t *value)
// {
//     char *endptr;
//     unsigned long parsed;

//     if ((text == RT_NULL) || (*text == '\0') || (*text == '-'))
//     {
//         return RT_FALSE;
//     }

//     parsed = strtoul(text, &endptr, 0);
//     if (*endptr != '\0')
//     {
//         return RT_FALSE;
//     }

//     *value = (rt_ubase_t)parsed;
//     return RT_TRUE;
// }

// static rt_bool_t smemlab_parse_ptr(const char *text, void **ptr)
// {
//     rt_ubase_t value;

//     if (!smemlab_parse_num(text, &value))
//     {
//         return RT_FALSE;
//     }

//     *ptr = (void *)value;
//     return RT_TRUE;
// }

// static rt_bool_t smemlab_is_pool_addr(const void *ptr)
// {
//     const rt_uint8_t *addr;
//     const rt_uint8_t *pool_begin;
//     const rt_uint8_t *pool_end;

//     if (ptr == RT_NULL)
//     {
//         return RT_FALSE;
//     }

//     addr = (const rt_uint8_t *)ptr;
//     pool_begin = g_smemlab_pool;
//     pool_end = g_smemlab_pool + sizeof(g_smemlab_pool);

//     return (addr >= pool_begin) && (addr < pool_end);
// }

// static rt_bool_t smemlab_range_in_pool(const void *ptr, rt_size_t len)
// {
//     const rt_uint8_t *addr;
//     const rt_uint8_t *pool_end;

//     if (!smemlab_is_pool_addr(ptr))
//     {
//         return RT_FALSE;
//     }

//     addr = (const rt_uint8_t *)ptr;
//     pool_end = g_smemlab_pool + sizeof(g_smemlab_pool);

//     return len <= (rt_size_t)(pool_end - addr);
// }

// static int smemlab_parse_active_ptr(const char *text, void **ptr, rt_size_t *block_size)
// {
//     void *parsed_ptr;
//     rt_size_t size;

//     if (!smemlab_parse_ptr(text, &parsed_ptr))
//     {
//         rt_kprintf("invalid pointer: %s\n", text);
//         return -RT_EINVAL;
//     }

//     if (!smemlab_is_pool_addr(parsed_ptr))
//     {
//         rt_kprintf("pointer 0x%08lx is outside smemlab raw pool\n",
//                    (unsigned long)(rt_uintptr_t)parsed_ptr);
//         return -RT_EINVAL;
//     }

//     if (!memlab_smem_is_valid_ptr(g_smemlab, parsed_ptr))
//     {
//         rt_kprintf("pointer 0x%08lx is not an active smemlab allocation\n",
//                    (unsigned long)(rt_uintptr_t)parsed_ptr);
//         return -RT_EINVAL;
//     }

//     size = memlab_smem_block_size(g_smemlab, parsed_ptr);
//     if (ptr != RT_NULL)
//     {
//         *ptr = parsed_ptr;
//     }
//     if (block_size != RT_NULL)
//     {
//         *block_size = size;
//     }

//     return RT_EOK;
// }

// static void smemlab_hexdump(const void *buf, rt_size_t len)
// {
//     if ((buf == RT_NULL) || (len == 0))
//     {
//         return;
//     }

//     _LOG_HEX((uint32_t)(rt_uintptr_t)buf, (uint8_t *)buf, (uint32_t)len);
// }

// static int smemlab_cmd_init(void)
// {
//     if (g_smemlab_inited && (g_smemlab != RT_NULL))
//     {
//         memlab_smem_detach(g_smemlab);
//         g_smemlab = RT_NULL;
//         g_smemlab_inited = RT_FALSE;
//     }

//     rt_memset(g_smemlab_pool, 0, sizeof(g_smemlab_pool));

//     g_smemlab = memlab_smem_init("smemlab", g_smemlab_pool, sizeof(g_smemlab_pool));
//     if (g_smemlab == RT_NULL)
//     {
//         rt_kprintf("smemlab init failed\n");
//         return -RT_ERROR;
//     }

//     g_smemlab_inited = RT_TRUE;

//     rt_kprintf("smemlab initialized\n");
//     rt_kprintf("raw_pool    : 0x%08lx - 0x%08lx (%lu bytes)\n",
//                (unsigned long)(rt_uintptr_t)g_smemlab_pool,
//                (unsigned long)(rt_uintptr_t)(g_smemlab_pool + sizeof(g_smemlab_pool) - 1),
//                (unsigned long)sizeof(g_smemlab_pool));
//     rt_kprintf("object      : 0x%08lx\n", (unsigned long)(rt_uintptr_t)g_smemlab);
//     rt_kprintf("managed_ptr : 0x%08lx\n", (unsigned long)g_smemlab->address);
//     rt_kprintf("usable      : %lu bytes\n", (unsigned long)g_smemlab->total);

//     return RT_EOK;
// }

// static int smemlab_cmd_info(void)
// {
//     rt_ubase_t managed_end;

//     managed_end = g_smemlab->total ? (g_smemlab->address + g_smemlab->total - 1) : g_smemlab->address;

//     rt_kprintf("raw_pool    : 0x%08lx - 0x%08lx (%lu bytes)\n",
//                (unsigned long)(rt_uintptr_t)g_smemlab_pool,
//                (unsigned long)(rt_uintptr_t)(g_smemlab_pool + sizeof(g_smemlab_pool) - 1),
//                (unsigned long)sizeof(g_smemlab_pool));
//     rt_kprintf("object      : 0x%08lx\n", (unsigned long)(rt_uintptr_t)g_smemlab);
//     rt_kprintf("managed_ptr : 0x%08lx\n", (unsigned long)g_smemlab->address);
//     rt_kprintf("managed_end : 0x%08lx\n", (unsigned long)managed_end);
//     rt_kprintf("usable      : %lu bytes\n", (unsigned long)g_smemlab->total);
//     rt_kprintf("used        : %lu bytes\n", (unsigned long)g_smemlab->used);
//     rt_kprintf("max_used    : %lu bytes\n", (unsigned long)g_smemlab->max);
//     rt_kprintf("note        : usable size is smaller than 1024 because metadata lives inside the raw pool\n");

//     return RT_EOK;
// }

// static int smemlab_cmd_alloc(const char *size_text)
// {
//     rt_ubase_t size_value;
//     void *ptr;
//     rt_size_t block_size;

//     if (!smemlab_parse_num(size_text, &size_value))
//     {
//         rt_kprintf("invalid size: %s\n", size_text);
//         return -RT_EINVAL;
//     }

//     ptr = memlab_smem_alloc(g_smemlab, (rt_size_t)size_value);
//     if (ptr == RT_NULL)
//     {
//         rt_kprintf("alloc failed for %lu bytes\n", (unsigned long)size_value);
//         return -RT_ENOMEM;
//     }

//     block_size = memlab_smem_block_size(g_smemlab, ptr);
//     rt_kprintf("alloc ok: ptr=0x%08lx request=%lu block=%lu\n",
//                (unsigned long)(rt_uintptr_t)ptr,
//                (unsigned long)size_value,
//                (unsigned long)block_size);

//     return RT_EOK;
// }

// static int smemlab_cmd_realloc(const char *ptr_text, const char *size_text)
// {
//     void *old_ptr;
//     void *new_ptr;
//     rt_size_t old_block_size;
//     rt_size_t new_block_size;
//     rt_ubase_t size_value;
//     int ret;

//     ret = smemlab_parse_active_ptr(ptr_text, &old_ptr, &old_block_size);
//     if (ret != RT_EOK)
//     {
//         return ret;
//     }

//     if (!smemlab_parse_num(size_text, &size_value))
//     {
//         rt_kprintf("invalid size: %s\n", size_text);
//         return -RT_EINVAL;
//     }

//     new_ptr = memlab_smem_realloc(g_smemlab, old_ptr, (rt_size_t)size_value);
//     if (size_value == 0)
//     {
//         rt_kprintf("realloc freed: old_ptr=0x%08lx old_block=%lu\n",
//                    (unsigned long)(rt_uintptr_t)old_ptr,
//                    (unsigned long)old_block_size);
//         return RT_EOK;
//     }

//     if (new_ptr == RT_NULL)
//     {
//         rt_kprintf("realloc failed: old_ptr=0x%08lx requested=%lu old_block=%lu\n",
//                    (unsigned long)(rt_uintptr_t)old_ptr,
//                    (unsigned long)size_value,
//                    (unsigned long)old_block_size);
//         return -RT_ENOMEM;
//     }

//     new_block_size = memlab_smem_block_size(g_smemlab, new_ptr);
//     rt_kprintf("realloc ok: old_ptr=0x%08lx new_ptr=0x%08lx request=%lu new_block=%lu\n",
//                (unsigned long)(rt_uintptr_t)old_ptr,
//                (unsigned long)(rt_uintptr_t)new_ptr,
//                (unsigned long)size_value,
//                (unsigned long)new_block_size);

//     return RT_EOK;
// }

// static int smemlab_cmd_free(const char *ptr_text)
// {
//     void *ptr;
//     rt_size_t block_size;
//     int ret;

//     ret = smemlab_parse_active_ptr(ptr_text, &ptr, &block_size);
//     if (ret != RT_EOK)
//     {
//         return ret;
//     }

//     memlab_smem_free(ptr);
//     rt_kprintf("free ok: ptr=0x%08lx block=%lu\n",
//                (unsigned long)(rt_uintptr_t)ptr,
//                (unsigned long)block_size);

//     return RT_EOK;
// }

// static int smemlab_cmd_fill(const char *ptr_text, const char *len_text, const char *byte_text)
// {
//     void *ptr;
//     rt_size_t block_size;
//     rt_ubase_t len_value;
//     rt_ubase_t byte_value;
//     int ret;

//     ret = smemlab_parse_active_ptr(ptr_text, &ptr, &block_size);
//     if (ret != RT_EOK)
//     {
//         return ret;
//     }

//     if (!smemlab_parse_num(len_text, &len_value))
//     {
//         rt_kprintf("invalid len: %s\n", len_text);
//         return -RT_EINVAL;
//     }

//     if (!smemlab_parse_num(byte_text, &byte_value) || (byte_value > 0xFF))
//     {
//         rt_kprintf("invalid byte value: %s\n", byte_text);
//         return -RT_EINVAL;
//     }

//     if (len_value > block_size)
//     {
//         rt_kprintf("fill len %lu exceeds block size %lu\n",
//                    (unsigned long)len_value,
//                    (unsigned long)block_size);
//         return -RT_EINVAL;
//     }

//     rt_memset(ptr, (int)byte_value, (rt_size_t)len_value);
//     rt_kprintf("fill ok: ptr=0x%08lx len=%lu value=0x%02lx\n",
//                (unsigned long)(rt_uintptr_t)ptr,
//                (unsigned long)len_value,
//                (unsigned long)byte_value);

//     return RT_EOK;
// }

// static int smemlab_cmd_dump_pool(int argc, char **argv)
// {
//     rt_ubase_t offset_value;
//     rt_ubase_t len_value;
//     rt_size_t actual_len;
//     rt_uint8_t *dump_ptr;

//     offset_value = 0;
//     len_value = sizeof(g_smemlab_pool);

//     if (argc >= 4)
//     {
//         if (!smemlab_parse_num(argv[3], &offset_value))
//         {
//             rt_kprintf("invalid offset: %s\n", argv[3]);
//             return -RT_EINVAL;
//         }
//     }

//     if (argc >= 5)
//     {
//         if (!smemlab_parse_num(argv[4], &len_value))
//         {
//             rt_kprintf("invalid len: %s\n", argv[4]);
//             return -RT_EINVAL;
//         }
//     }

//     if (offset_value > sizeof(g_smemlab_pool))
//     {
//         rt_kprintf("offset %lu exceeds raw pool size %lu\n",
//                    (unsigned long)offset_value,
//                    (unsigned long)sizeof(g_smemlab_pool));
//         return -RT_EINVAL;
//     }

//     actual_len = (rt_size_t)len_value;
//     if (actual_len > (sizeof(g_smemlab_pool) - (rt_size_t)offset_value))
//     {
//         actual_len = sizeof(g_smemlab_pool) - (rt_size_t)offset_value;
//         rt_kprintf("dump len clipped to %lu bytes\n", (unsigned long)actual_len);
//     }

//     dump_ptr = g_smemlab_pool + offset_value;
//     rt_kprintf("dump pool: raw_base=0x%08lx addr=0x%08lx offset=%lu len=%lu\n",
//                (unsigned long)(rt_uintptr_t)g_smemlab_pool,
//                (unsigned long)(rt_uintptr_t)dump_ptr,
//                (unsigned long)offset_value,
//                (unsigned long)actual_len);

//     if (actual_len == 0)
//     {
//         rt_kprintf("no bytes to dump\n");
//         return RT_EOK;
//     }

//     smemlab_hexdump(dump_ptr, actual_len);
//     return RT_EOK;
// }

// static int smemlab_cmd_dump_ptr(const char *ptr_text, const char *len_text)
// {
//     void *ptr;
//     rt_size_t block_size;
//     rt_ubase_t len_value;
//     rt_size_t actual_len;
//     int ret;

//     ret = smemlab_parse_active_ptr(ptr_text, &ptr, &block_size);
//     if (ret != RT_EOK)
//     {
//         return ret;
//     }

//     if (!smemlab_parse_num(len_text, &len_value))
//     {
//         rt_kprintf("invalid len: %s\n", len_text);
//         return -RT_EINVAL;
//     }

//     actual_len = (rt_size_t)len_value;
//     if (actual_len > block_size)
//     {
//         actual_len = block_size;
//         rt_kprintf("dump len clipped to block size %lu\n", (unsigned long)actual_len);
//     }

//     if (!smemlab_range_in_pool(ptr, actual_len))
//     {
//         rt_kprintf("dump range exceeds raw pool\n");
//         return -RT_EINVAL;
//     }

//     rt_kprintf("dump ptr: addr=0x%08lx len=%lu block=%lu\n",
//                (unsigned long)(rt_uintptr_t)ptr,
//                (unsigned long)actual_len,
//                (unsigned long)block_size);

//     if (actual_len == 0)
//     {
//         rt_kprintf("no bytes to dump\n");
//         return RT_EOK;
//     }

//     smemlab_hexdump(ptr, actual_len);
//     return RT_EOK;
// }

// static int smemlab_cmd_dump(int argc, char **argv)
// {
//     if (argc < 4)
//     {
//         if ((argc == 3) && (rt_strcmp(argv[2], "pool") == 0))
//         {
//             return smemlab_cmd_dump_pool(argc, argv);
//         }

//         smemlab_usage();
//         return -RT_EINVAL;
//     }

//     if (rt_strcmp(argv[2], "pool") == 0)
//     {
//         if (argc > 5)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_dump_pool(argc, argv);
//     }

//     if (rt_strcmp(argv[2], "ptr") == 0)
//     {
//         if (argc != 5)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_dump_ptr(argv[3], argv[4]);
//     }

//     rt_kprintf("unknown dump target: %s\n", argv[2]);
//     return -RT_EINVAL;
// }

// static int smemlab_cmd(int argc, char **argv)
// {
//     if (argc < 2)
//     {
//         smemlab_usage();
//         return -RT_EINVAL;
//     }

//     if (rt_strcmp(argv[1], "help") == 0)
//     {
//         smemlab_usage();
//         return RT_EOK;
//     }

//     if (rt_strcmp(argv[1], "init") == 0)
//     {
//         return smemlab_cmd_init();
//     }

//     if (!smemlab_ready())
//     {
//         return -RT_ERROR;
//     }

//     if (rt_strcmp(argv[1], "info") == 0)
//     {
//         return smemlab_cmd_info();
//     }

//     if (rt_strcmp(argv[1], "alloc") == 0)
//     {
//         if (argc != 3)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_alloc(argv[2]);
//     }

//     if (rt_strcmp(argv[1], "realloc") == 0)
//     {
//         if (argc != 4)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_realloc(argv[2], argv[3]);
//     }

//     if (rt_strcmp(argv[1], "free") == 0)
//     {
//         if (argc != 3)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_free(argv[2]);
//     }

//     if (rt_strcmp(argv[1], "fill") == 0)
//     {
//         if (argc != 5)
//         {
//             smemlab_usage();
//             return -RT_EINVAL;
//         }

//         return smemlab_cmd_fill(argv[2], argv[3], argv[4]);
//     }

//     if (rt_strcmp(argv[1], "dump") == 0)
//     {
//         return smemlab_cmd_dump(argc, argv);
//     }

//     if (rt_strcmp(argv[1], "trace") == 0)
//     {
//         memlab_smem_trace(g_smemlab);
//         return RT_EOK;
//     }

//     if (rt_strcmp(argv[1], "check") == 0)
//     {
//         int ret;

//         ret = memlab_smem_check(g_smemlab);
//         if (ret == RT_EOK)
//         {
//             rt_kprintf("check ok\n");
//         }
//         return ret;
//     }

//     rt_kprintf("unknown subcommand: %s\n", argv[1]);
//     smemlab_usage();
//     return -RT_EINVAL;
// }

// MSH_CMD_EXPORT_ALIAS(smemlab_cmd, smemlab, small memory lab command);

// #endif /* defined(RT_USING_SMALL_MEM) */
