# 小内存管理算法心得分享

这篇文档的目标不是把 `malloc/free` 的每一行代码背下来，而是从一个最小模型开始，逐步理解 RT-Thread small memory 为什么这样设计：

- 为什么每个内存块前面要放一个块头。
- 为什么块头里记录 `next/prev`。
- 为什么 used/free 状态可以塞进指针最低 bit。
- 为什么释放时必须合并相邻空闲块。
- `lfree`、`heap_end` 这些字段到底在解决什么问题。

本文对应当前工程里的实验代码：

- `applications/mem/rtmem.c`：当前实验使用的 RT-Thread small memory 实现。
- `applications/mem/rtmem_cli.c`：FinSH/MSH 命令 `rtmemlab`，用于初始化、分配、释放、查看块列表和 dump 原始内存。
- `rt-thread/src/mem.c`：RT-Thread 原始 small memory 实现，可作为最终源码对照。

## 1. 内存管理要解决什么

程序直接面对一段连续内存时，只知道两个信息：

- 起始地址在哪里。
- 总共有多少字节。

但是动态分配需要回答更多问题：

- 哪些区域已经被分配出去。
- 哪些区域还空闲。
- 一个用户指针对应的块到底有多大。
- 释放一个块后，能不能和前后空闲块合并。
- 下一次分配时，应该从哪里开始找空闲空间。

如果这些信息没有设计好，常见问题会很快出现：

- **内存泄露**：分配出去的块不再释放，空闲空间越来越少。
- **内部碎片**：实际请求很小，但因为对齐或最小块大小，分配出去的块更大。
- **外部碎片**：总空闲空间足够，但被分散成多个小块，无法满足一次较大的连续分配。
- **越界破坏**：用户写过头，覆盖块头，导致后续分配/释放链表损坏。

small memory 的核心思路是：把一段连续内存切成多个块，每个块前面放一个 metadata 头，通过块头把整段堆串起来。

## 2. 第一版：只支持定长分配

先从最简单的模型开始：整个内存池只切成固定大小的块，比如每块 32 字节。

管理结构可以非常简单：

```c
struct fixed_pool {
    uint8_t *base;
    size_t block_size;
    size_t block_count;
    uint8_t used_map[];
};
```

初始化时：

- 把 `base` 指向内存池起始地址。
- 按 `block_size` 切成 `block_count` 个块。
- `used_map[i] = 0` 表示第 `i` 块空闲。
- `used_map[i] = 1` 表示第 `i` 块已使用。

分配时只做 first-fit：

1. 从第 0 个块开始扫描 `used_map`。
2. 找到第一个空闲块。
3. 标记为已使用。
4. 返回 `base + i * block_size`。

释放时：

1. 根据用户指针算出块下标。
2. 检查指针是否落在池内、是否对齐到块边界。
3. 把对应 `used_map[i]` 标记为空闲。

这个模型容易理解，但限制也明显：

- 每次只能分配固定大小。
- 请求 1 字节也要占 32 字节，内部碎片明显。
- 如果要支持 16/32/64/128 多种大小，就要维护多个池，复杂度会上升。

所以它只能帮助我们建立两个基本概念：**内存池**和**使用状态映射**。要支持通用 `malloc(size)`，还需要可变长分配。

## 3. 第二版：支持不定长分配

可变长分配的关键是：不能只在外部维护一张 bitmap，因为每个块大小都不同。每个块必须自己记录边界信息。

最小块头可以设计成：

```c
struct small_item {
    uintptr_t pool_ptr_and_used;
    size_t next;
    size_t prev;
};
```

含义如下：

- `pool_ptr_and_used`：保存所属内存池指针，并用最低 bit 表示 used/free。
- `next`：下一个块头相对 `heap_ptr` 的偏移。
- `prev`：上一个块头相对 `heap_ptr` 的偏移。

块的真实布局是：

```text
+------------------+---------------------+
| struct item      | user data           |
+------------------+---------------------+
^                  ^
block header       pointer returned to user
```

用户拿到的是 `item + sizeof(item)`，释放时再通过 `ptr - sizeof(item)` 找回块头。

### 为什么 used 可以放在指针最低 bit

内存池对象和块头都会按 `RT_ALIGN_SIZE` 对齐。对齐后的指针最低若干 bit 必然是 0。RT-Thread small memory 只借用最低 bit：

```c
#define MEM_USED(_mem)   ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x1)
#define MEM_FREED(_mem)  ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x0)
#define MEM_ISUSED(_mem) (((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (~MEM_MASK))
#define MEM_POOL(_mem)   ((struct rt_small_mem *)(((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & MEM_MASK))
```

也就是说：

- 读 `MEM_ISUSED(mem)`：看最低 bit 是否为 1。
- 读 `MEM_POOL(mem)`：把最低 bit 清掉，还原内存池对象指针。

这样一个字段同时保存了“属于哪个 heap”和“当前是否已使用”。

### 为什么 next/prev 存偏移而不是直接存指针

`next/prev` 存的是相对 `heap_ptr` 的偏移，例如：

```text
mem = heap_ptr + offset
next = heap_ptr + mem->next
prev = heap_ptr + mem->prev
```

这样计算块大小很直接：

```c
block_size = mem->next - current_offset - SIZEOF_STRUCT_MEM;
```

同时 `next/prev` 让整段 heap 形成一个按地址递增的双向链表。注意这里不是“空闲链表”，而是“所有块的链表”：USED 块和 FREE 块都在同一条链上。

## 4. small memory 的管理结构

当前实验代码里的核心结构在 `applications/mem/rtmem.c`。

整体管理对象：

```c
struct rt_small_mem {
    struct rt_memory parent;
    uint8_t *heap_ptr;
    struct rt_small_mem_item *heap_end;
    struct rt_small_mem_item *lfree;
    size_t mem_size_aligned;
};
```

关键字段：

- `heap_ptr`：真正被 small memory 管理的起始地址。
- `heap_end`：末尾哨兵块，不给用户分配，只用于结束遍历和边界判断。
- `lfree`：最低地址的空闲块，下一次 first-fit 从这里开始找。
- `mem_size_aligned`：对齐后的可用管理大小。

每个块头：

```c
struct rt_small_mem_item {
    uintptr_t pool_ptr;
    size_t next;
    size_t prev;
};
```

当前 `rtmem.c` 版本保持 RT-Thread small memory 的标准字段布局，没有额外的 magic 调试字段。

## 5. 初始化：为什么要有两个 item

`__rt_smem_init()` 做了几件事：

1. 把传入的原始内存按 `RT_ALIGN_SIZE` 对齐。
2. 在原始内存开头放 `struct rt_small_mem` 管理对象。
3. 后面的区域作为 heap。
4. 在 heap 起点创建第一个空闲块。
5. 在 heap 末尾创建一个 used 状态的哨兵块 `heap_end`。
6. `lfree` 指向第一个空闲块。

初始化后的逻辑布局：

```text
raw pool
+-------------------+---------------------+------------------+
| rt_small_mem obj  | first FREE item     | heap_end USED    |
+-------------------+---------------------+------------------+
                    ^                     ^
                    heap_ptr/lfree        heap_end
```

为什么需要 `heap_end`？

- 遍历块链表时有明确结束点。
- 分配时不会越过 heap 末尾。
- 释放合并时，最后一个真实块的 next 可以指向哨兵。
- 哨兵标记为 USED，可以避免被当作普通空闲块合并。

所以初始化时不是“一个空闲块就够了”，而是需要“一个真实空闲块 + 一个末尾哨兵块”。

## 6. alloc：first-fit、切分和 lfree 更新

`__rt_smem_alloc(m, size)` 的流程：

1. `size` 为 0 直接失败。
2. 请求大小按 `RT_ALIGN_SIZE` 对齐。
3. 如果小于最小块大小，提升到 `MIN_SIZE_ALIGNED`。
4. 从 `lfree` 开始沿 `next` 扫描。
5. 找到第一个 FREE 且容量足够的块。
6. 判断是否需要切分。
7. 标记当前块为 USED。
8. 如果当前块正好是 `lfree`，向后移动 `lfree` 到下一个空闲块。
9. 返回块头之后的用户地址。

### 为什么分配后还要切出一个 item

假设当前有一个 200 字节空闲块，请求 32 字节。如果直接整块分配，剩余空间就浪费了。

所以当剩余空间足够容纳：

- 一个新的 `struct rt_small_mem_item`
- 至少 `MIN_SIZE_ALIGNED` 的用户数据

就把原空闲块切成两块：

```text
before:
+---------------------------+
| FREE 200                  |
+---------------------------+

after alloc 32:
+----------+----------------+
| USED 32  | FREE remaining |
+----------+----------------+
```

切分时要更新：

- 新空闲块 `mem2->pool_ptr = FREE`。
- 新空闲块 `mem2->next = old mem->next`。
- 新空闲块 `mem2->prev = current ptr`。
- 当前块 `mem->next = ptr2`。
- 后继块的 `prev = ptr2`。

这些更新完成后，块链表仍然按地址顺序连接。

如果剩余空间太小，不能形成一个合法的新块，就整块分配出去。这个多出来但无法独立管理的空间就是内部碎片。

## 7. free：为什么必须合并

`__rt_smem_free(ptr)` 的流程：

1. 用户指针为 `NULL` 时直接返回。
2. 通过 `ptr - SIZEOF_STRUCT_MEM` 找回块头。
3. 检查块头 pool、used 状态和地址范围。
4. 把当前块标记为 FREE。
5. 如果当前块地址低于 `lfree`，更新 `lfree`。
6. 调用 `plug_holes()` 合并相邻空闲块。

如果释放后不合并，会产生明显外部碎片：

```text
+---------+---------+---------+
| FREE 32 | FREE 32 | FREE 32 |
+---------+---------+---------+
```

总空闲空间是 96 字节，但如果分配器只按单块查找，申请 80 字节仍然失败。合并后变成：

```text
+---------------------------+
| FREE 96                   |
+---------------------------+
```

下一次大块分配就可以成功。

## 8. plug_holes：双向合并放在哪里

`plug_holes()` 做两步：

1. 向后看 `nmem`，如果后一个块是 FREE，并且不是 `heap_end`，就把后一个块合并进当前块。
2. 向前看 `pmem`，如果前一个块是 FREE，就把当前块合并进前一个块。

顺序是“先向后，再向前”：

```text
case:
+------+---------+------+
| FREE | current | FREE |
+------+---------+------+

step 1: current + next
+------+----------------+
| FREE | current+next   |
+------+----------------+

step 2: prev + current
+-----------------------+
| prev+current+next     |
+-----------------------+
```

合并时最关键的是维护 `next/prev`：

- 当前块吞掉后一个块：`mem->next = nmem->next`，再把后继块的 `prev` 指回当前块。
- 前一个块吞掉当前块：`pmem->next = mem->next`，再把后继块的 `prev` 指回前一个块。

同时要维护 `lfree`：

- 如果 `lfree` 正好指向被合并掉的块，就要改到合并后的块。
- 如果释放了更低地址的块，`lfree` 要提前。

所以双向链表的更新集中在两个地方：

- `alloc` 切分时插入新块。
- `free/realloc shrink` 后调用 `plug_holes` 合并块。

## 9. realloc：缩小时切分，放大时重新分配

当前实验代码的 `__rt_smem_realloc()` 采用简单策略：

- `newsize == 0`：等价于 `free`。
- `rmem == NULL`：等价于 `alloc`。
- 新大小等于旧大小：原指针返回。
- 缩小时，如果剩余空间足够形成新空闲块，就切出一个 FREE 块并尝试合并。
- 放大时，重新 `alloc` 一个新块，复制旧内容，再释放旧块。

这个实现没有尝试“原地向后扩展”。这样逻辑更容易讲清楚，也方便通过 trace 观察。

## 10. 用 rtmemlab 观察内存变化

编译启用条件是 `RT_USING_SMALL_MEM`。当前工程 `rtconfig.h` 已启用：

```c
#define RT_USING_SMALL_MEM
#define RT_USING_SMALL_MEM_AS_HEAP
```

`applications/mem/SConscript` 会在该配置开启时编译 `MemLab` 组。

当前 `rtmemlab` 支持的子命令与 `applications/mem/rtmem_cli.c` 一致：

- `rtmemlab init`
- `rtmemlab info`
- `rtmemlab alloc <size>`
- `rtmemlab free <ptr>`
- `rtmemlab dump pool [offset] [len]`
- `rtmemlab dump ptr <ptr> <len>`
- `rtmemlab trace`
- `rtmemlab demo`
- `rtmemlab help`

### 10.1 初始化和基础信息

```text
rtmemlab init
rtmemlab info
rtmemlab trace
```

重点观察：

- `raw_pool` 是 1024 字节原始数组。
- `trace` 里的 `total` 小于原始池大小，因为池开头放了 `rt_small_mem` 管理对象，heap 内还要放块头和 `heap_end`。
- `trace` 里初始化后应该能看到一个 FREE 块。
- `heap_end` 不在普通 block list 输出里作为可分配块出现。

### 10.2 单次分配和释放

```text
rtmemlab alloc 16
rtmemlab trace
rtmemlab free <ptr>
rtmemlab trace
```

观察点：

- 分配 16 字节后，原来的大 FREE 块被切成 `USED + FREE`。
- 用户指针是块头之后的地址，不是块头地址。
- 释放后，`USED` 重新变为 `FREE`，并和后一个 FREE 合并。

### 10.3 连续分配制造碎片

```text
rtmemlab init
rtmemlab alloc 16
rtmemlab alloc 32
rtmemlab alloc 64
rtmemlab trace
```

假设返回的三个指针分别是 `p1/p2/p3`，释放中间块：

```text
rtmemlab free <p2>
rtmemlab trace
```

这时中间出现一个 FREE 块，但它前后都是 USED，无法合并。这就是外部碎片的基本形态。

### 10.4 观察相邻释放后的合并

重新初始化后连续分配 4 个块：

```text
rtmemlab init
rtmemlab alloc 16
rtmemlab alloc 16
rtmemlab alloc 16
rtmemlab alloc 16
rtmemlab trace
```

假设第 3、4 个指针是 `p3/p4`：

```text
rtmemlab free <p4>
rtmemlab trace
rtmemlab free <p3>
rtmemlab trace
```

观察点：

- 释放 `p4` 时，它会和后面的尾部 FREE 块合并。
- 再释放 `p3` 时，它会和后面的 FREE 块继续合并。
- 如果 `p2` 仍然 USED，合并不会跨过 `p2`。

### 10.5 dump 原始内存

```text
rtmemlab dump pool 0 128
rtmemlab dump ptr <ptr> 32
```

`dump pool` 用来看原始池内的管理对象、块头、用户数据实际如何排列。

`dump ptr` 从用户指针开始 dump，只能看到用户数据区。如果要看块头，需要用 `dump pool` 从更早的 offset 开始观察。

### 10.6 一键演示流程

```text
rtmemlab demo
```

`demo` 会自动执行一组固定分配/释放动作，并在每一步打印 trace，适合快速观察：

- 分配后链表如何切分为 `USED + FREE`。
- 中间释放后如何形成外部碎片。
- 连续释放相邻块后如何被 `plug_holes()` 合并。

## 11. 源码带读顺序

建议按下面顺序读，不要一开始就从头到尾扫源码。

### 11.1 先看结构和宏

文件：`applications/mem/rtmem.c`

先理解：

- `struct rt_small_mem`
- `struct rt_small_mem_item`
- `MIN_SIZE`
- `MEM_MASK`
- `MEM_USED`
- `MEM_FREED`
- `MEM_ISUSED`
- `MEM_POOL`
- `MEM_SIZE`
- `SIZEOF_STRUCT_MEM`

读完这一部分，应该能回答：

- 用户指针和块头地址差多少。
- 如何从块头知道所属 heap。
- 如何判断块是否 used。
- 如何通过 `next` 算出当前块用户区大小。

### 11.2 再看 init

函数：`__rt_smem_init()`

重点问题：

- 原始池开头为什么要放 `struct rt_small_mem`。
- `begin_align/end_align` 为什么都要对齐。
- `mem_size = end_align - begin_align - 2 * SIZEOF_STRUCT_MEM` 为什么要减两个块头。
- 第一个块为什么是 FREE。
- `heap_end` 为什么是 USED。
- `lfree` 为什么初始化为第一个块。

### 11.3 再看 alloc

函数：`__rt_smem_alloc()`

重点问题：

- 为什么要对齐请求大小。
- 为什么小于 `MIN_SIZE_ALIGNED` 时要抬高。
- first-fit 从哪里开始。
- 什么条件下切分。
- 切分时 `next/prev` 分别如何改。
- `lfree` 什么时候向后移动。

### 11.4 再看 free 和 plug_holes

函数：

- `__rt_smem_free()`
- `plug_holes()`

重点问题：

- 为什么 free 能从用户指针反推出块头。
- 为什么释放后先更新 `lfree`，再合并。
- 向后合并和向前合并分别解决什么情况。
- 为什么不能和 `heap_end` 合并。

### 11.5 最后看 realloc/trace

函数：

- `__rt_smem_realloc()`
- `rtmemlab_trace()`（在 `applications/mem/rtmem_cli.c` 中）

这些函数不是理解 small memory 主流程的第一入口，但适合验证理解：

- `realloc` 展示缩小块时如何切分并合并。
- `trace` 把链表状态打印出来，适合和每次命令后的布局变化对照。

## 12. 使用 small memory 的建议

基于上面的原理，使用这类小内存管理算法时要注意：

- 频繁分配/释放不同大小的块会增加外部碎片。
- 尽量让生命周期相近的对象一起分配、一起释放。
- 高频固定大小对象更适合单独使用 mempool，而不是反复走通用 small heap。
- 不要越界写用户 buffer，块头通常就在用户指针前面，越界很容易破坏堆结构。
- `free` 必须传回原始分配得到的用户指针，不能传中间地址。
- `realloc` 可能返回新地址，调用方必须使用返回值更新自己的指针。
- `lfree` 只是加速 first-fit，不表示只有它一个空闲块。

small memory 的设计并不复杂，但它把几个关键问题放在了一起：块边界、使用状态、链表维护、对齐、切分、合并。只要能通过 `rtmemlab trace` 把每次分配释放后的块列表画出来，RT-Thread 这套 small memory 源码就基本能读懂。
