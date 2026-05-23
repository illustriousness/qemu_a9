## 开场白
大家好 我是xxx 今天带来的内容是：拆解分析rt-thread中小内存管理算法的底层机制
我们将从一个最小模型开始，逐步理解 RT-Thread small memory 为什么这样设计
## ppt 开始 
## 1. 内存物理模型介绍 
在mcu中 内存一般是这样的
| data | bss | stack | heap |
上述不一定是固定行为 可以根据链接脚本调整
- `data` 一般是 程序中预先定义的常量 比如 int a =100; 
- `bss` 一般是 没有定义的常量  比如 int a； 大部分情况下启动汇编会将这段区域清0
- `stack` 一般是函数中的变量和出入栈使用 `bss_end` 一般会定义到stack 结束
- `heap` 一般用于rtt的内存管理


```c
extern int __bss_end;
#define HEAP_BEGIN      ((void*)&__bss_end)
#define HEAP_END        (void*)(0x20000000 + 64 * 1024)
    /* initialize system heap */
    rt_system_heap_init(HEAP_BEGIN, HEAP_END);
```
这个函数会把 [HEAP_BEGIN, HEAP_END) 注册为系统堆，并按当前内存管理算法完成初始化；
后续 rt_malloc/rt_free/rt_realloc 都在这段区域内进行分配与回收。

## 2. 为什么需要内存管理 
因为内存不够 不然的话 我可以直接在代码编译前 就定义 
这样的话就是静态内存 
但是 我们的内存不够 所以就去分时复用
a临时用完后释放 b就可以再使用了 

## 3. 内存管理要解决什么

程序直接面对一段连续内存时，只需要知道
- 起始地址在哪里。
- 总共有多少字节。

但是动态分配就需要考虑很多了 

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

## 4. 内存管理的实现方式

## 4.1 第一版：只支持定长分配

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

**初始化时：**

- 把 `base` 指向内存池起始地址。
- 按 `block_size` 切成 `block_count` 个块。
- `used_map[i] = 0` 表示第 `i` 块空闲。
- `used_map[i] = 1` 表示第 `i` 块已使用。

**分配时只做 first-fit：**

1. 从第 0 个块开始扫描 `used_map`。
2. 找到第一个空闲块。
3. 标记为已使用。
4. 返回 `base + i * block_size`。

**释放时：**

1. 根据用户指针算出块下标。
2. 检查指针是否落在池内、是否对齐到块边界。
3. 把对应 `used_map[i]` 标记为空闲。

这个模型容易理解，但限制也明显：

- 每次只能分配固定大小。
- 请求 1 字节也要占 32 字节，内部碎片明显。
- 如果要支持 16/32/64/128 多种大小，就要维护多个池，复杂度会上升。

所以它只能帮助我们建立两个基本概念：**内存池**和**使用状态映射**。要支持通用 `malloc(size)`，还需要可变长分配。

## 4.2 第二版：支持不定长分配

#### 结构体定义 
块头结构：

```c
typedef struct
{
    rt_uint8_t used;
    rt_size_t next;
    rt_size_t prev;
} varmem_item_t;
```

堆管理结构：

```c
typedef struct
{
    rt_uint8_t *heap_ptr;
    varmem_item_t *heap_end;
    rt_size_t mem_size;
    rt_size_t used_size;
    rt_size_t max_used_size;
    rt_bool_t inited;
} varmem_heap_t;
```
**注意 这里的next 和 prev 实际是内存堆指针的偏移**
我们把每个分配出去的内存项 使用链表串起来 每一个内存项中前面保存原始信息 后面的空间给用户

#### 初始化时

`varmem_init_heap()` 的主要步骤：

1. 对齐原始池起止地址。
2. 检查空间是否至少能容纳：两个块头 + 最小用户区。
3. 计算 `mem_size = end - begin - 2 * VARMEM_ITEM_SIZE`。
4. 清零状态并建立初始链表。

#### 初始化后布局

```text
[first item header][free user area: mem_size][heap_end header]
```

其中：

- `first->used = 0`（初始唯一可分配 FREE 块）。
- `first->next = mem_size + VARMEM_ITEM_SIZE`（指向 `heap_end`）。
- `heap_end->used = 1`（哨兵块，不可分配）。

#### 不定长分配流程

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

1. 参数检查：`size == 0` 或未初始化直接失败。
2. 对齐请求：`size = RT_ALIGN(size, RT_ALIGN_SIZE)`。
3. 下限保护：小于 `VARMEM_MIN_SIZE` 时提升到最小值。
4. 上限保护：`size > mem_size` 直接失败。
5. 从 `offset = 0` 开始扫描块链表（first-fit from heap head）。
6. 对每个块计算 `block_size`，跳过 USED 或容量不足的块。
7. 命中后按“是否可切分”分两条路径处理。

块用户区大小计算公式：

```c
 item_user_size = item->next - item_offset - VARMEM_ITEM_SIZE;
```

可切分条件：

```c
block_size >= size + VARMEM_ITEM_SIZE + VARMEM_MIN_SIZE
```

满足时：

- 在 `split_offset = offset + VARMEM_ITEM_SIZE + size` 新建 `split` 空闲块。
- `split->next = item->next`，`split->prev = offset`。
- `item->next = split_offset`，并修复后继块的 `prev`。
- `used_size += size + VARMEM_ITEM_SIZE`（只记本次实际占用）。

不满足时（剩余空间太小，无法形成合法新块）：

- 整块分配给用户。
- `used_size += item->next - offset`（整块占用都计入）。

最后统一：

- `item->used = 1`。
- 更新 `max_used_size`。
- 返回用户指针：`(rt_uint8_t *)item + VARMEM_ITEM_SIZE`。



#### 释放与合并
1. `ptr == NULL` 或未初始化返回错误。
2. `item = ptr - VARMEM_ITEM_SIZE` 找回块头。
3. 若块当前不是 USED，返回错误。
4. 标记 FREE，并从 `used_size` 扣除该块占用。
5. 调用 `varmem_plug_holes(item)` 合并相邻空闲块。

`varmem_plug_holes(item)` 做两步：

1. 向后合并：后继是 FREE 且不是 `heap_end` 时，吞并后继块。
2. 向前合并：前驱是 FREE 时，让前驱吞并当前块。
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

相邻 FREE 块逐步并成更大连续块。

**过渡语**
现在 我们似乎实现了一个不错的内存分配算法
但是 注意 
1. 我们每次 调用malloc的时候 扫描起点固定是 `heap_ptr`，不是最低空闲块。
遍历的过程中 有的是空闲的 有的是被使用 
由于实时系统中对时间的要求非常严格，我们必须对其进行优化
这时候我们引入一个`lfree` 来表示举例heap ptr 最近的空闲快

1. 我们定义了一个静态的堆管理结构体 在每次free的 时候获取当前内存的管理信息
   rtt 中采用了一种更优雅的方式 把 used 改为 pool_ptr
   pool_ptr:小内存对象地址。如果内存块的最后一位为1，则标记为使用。如果内存块的最后一位为0，则标记为未使用。可以通过该地址计算快速获得小内存算法结构体成员。
  
得益与 内存池对象和块头都会按 `RT_ALIGN_SIZE` 对齐。对齐后的指针最低若干 bit 必然是 0。RT-Thread small memory 只借用最低 bit

## 5. 揭开 RT-Thread 小内存管理的面纱

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

### 初始化

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
### alloc：first-fit、切分和 lfree 更新

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

切分时要更新：

- 新空闲块 `mem2->pool_ptr = FREE`。
- 新空闲块 `mem2->next = old mem->next`。
- 新空闲块 `mem2->prev = current ptr`。
- 当前块 `mem->next = ptr2`。
- 后继块的 `prev = ptr2`。

这些更新完成后，块链表仍然按地址顺序连接。

如果剩余空间太小，不能形成一个合法的新块，就整块分配出去。这个多出来但无法独立管理的空间就是内部碎片。

### free

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

### plug_holes：双向合并放在哪里

`plug_holes()` 做两步：

1. 向后看 `nmem`，如果后一个块是 FREE，并且不是 `heap_end`，就把后一个块合并进当前块。
2. 向前看 `pmem`，如果前一个块是 FREE，就把当前块合并进前一个块。

顺序是“先向后，再向前”：

合并时最关键的是维护 `next/prev`：

- 当前块吞掉后一个块：`mem->next = nmem->next`，再把后继块的 `prev` 指回当前块。
- 前一个块吞掉当前块：`pmem->next = mem->next`，再把后继块的 `prev` 指回前一个块。

同时要维护 `lfree`：

- 如果 `lfree` 正好指向被合并掉的块，就要改到合并后的块。
- 如果释放了更低地址的块，`lfree` 要提前。

所以双向链表的更新集中在两个地方：

- `alloc` 切分时插入新块。
- `free/realloc shrink` 后调用 `plug_holes` 合并块。

### realloc：缩小时切分，放大时重新分配

当前实验代码的 `__rt_smem_realloc()` 采用简单策略：

- `newsize == 0`：等价于 `free`。
- `rmem == NULL`：等价于 `alloc`。
- 新大小等于旧大小：原指针返回。
- 缩小时，如果剩余空间足够形成新空闲块，就切出一个 FREE 块并尝试合并。
- 放大时，重新 `alloc` 一个新块，复制旧内容，再释放旧块。

## 使用 small memory 的建议

基于上面的原理，使用这类小内存管理算法时要注意：

- 频繁分配/释放不同大小的块会增加外部碎片。
- 尽量让生命周期相近的对象一起分配、一起释放。
- 高频固定大小对象更适合单独使用 mempool，而不是反复走通用 small heap。
- 不要越界写用户 buffer，块头通常就在用户指针前面，越界很容易破坏堆结构。
- `free` 必须传回原始分配得到的用户指针，不能传中间地址。
- `realloc` 可能返回新地址，调用方必须使用返回值更新自己的指针。
- `lfree` 只是加速 first-fit，不表示只有它一个空闲块。

small memory 的设计并不复杂，但它把几个关键问题放在了一起：块边界、使用状态、链表维护、对齐、切分、合并。
只要能通过 `rtmemlab trace` 把每次分配释放后的块列表画出来，RT-Thread 这套 small memory 源码就基本能读懂。

## ppt结束

----
以下内容为ppt 格式要求
1. 字体 Noto Mono