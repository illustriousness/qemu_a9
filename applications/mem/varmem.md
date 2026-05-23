# 不定长内存分配（varmem.c）说明

这份文档只按 `applications/mem/varmem.c` 当前实现来介绍不定长分配，不混入 `rt_small_mem` 的 `lfree` 优化版本。

## 1. 目标与特点

`varmem.c` 是一个教学版不定长分配器，核心特点：

- 每个块都有头部，头部保存 `used/next/prev`。
- 所有块（USED/FREE）在一条按地址递增的双向链表里。
- 分配使用 first-fit，但**总是从堆头开始扫描**，不使用 `lfree`。
- 释放时调用合并逻辑，减少外部碎片。

对应命令在 `applications/mem/varmem.c` 里注册为 `varmem`：

- `varmem init`
- `varmem info`
- `varmem alloc <size>`
- `varmem free <ptr>`
- `varmem trace`
- `varmem demo`
- `varmem help`

## 2. 关键数据结构

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

实现里依赖两个尺寸常量：

- `VARMEM_ITEM_SIZE = RT_ALIGN(sizeof(varmem_item_t), RT_ALIGN_SIZE)`：块头大小（含对齐）。
- `VARMEM_MIN_SIZE = RT_ALIGN(sizeof(rt_uintptr_t), RT_ALIGN_SIZE)`：最小用户区大小。

块用户区大小计算公式：

```c
item_user_size = item->next - item_offset - VARMEM_ITEM_SIZE;
```

## 3. 初始化布局

`varmem_init_heap()` 的主要步骤：

1. 对齐原始池起止地址。
2. 检查空间是否至少能容纳：两个块头 + 最小用户区。
3. 计算 `mem_size = end - begin - 2 * VARMEM_ITEM_SIZE`。
4. 清零状态并建立初始链表。

初始化后布局：

```text
[first item header][free user area: mem_size][heap_end header]
```

其中：

- `first->used = 0`（初始唯一可分配 FREE 块）。
- `first->next = mem_size + VARMEM_ITEM_SIZE`（指向 `heap_end`）。
- `heap_end->used = 1`（哨兵块，不可分配）。

## 4. 不定长分配流程（对应 varmem_alloc_block）

`varmem_alloc_block(size)` 的实现流程：

1. 参数检查：`size == 0` 或未初始化直接失败。
2. 对齐请求：`size = RT_ALIGN(size, RT_ALIGN_SIZE)`。
3. 下限保护：小于 `VARMEM_MIN_SIZE` 时提升到最小值。
4. 上限保护：`size > mem_size` 直接失败。
5. 从 `offset = 0` 开始扫描块链表（first-fit from heap head）。
6. 对每个块计算 `block_size`，跳过 USED 或容量不足的块。
7. 命中后按“是否可切分”分两条路径处理。

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

## 5. 释放与合并

1. `ptr == NULL` 或未初始化返回错误。
2. `item = ptr - VARMEM_ITEM_SIZE` 找回块头。
3. 若块当前不是 USED，返回错误。
4. 标记 FREE，并从 `used_size` 扣除该块占用。
5. 调用 `varmem_plug_holes(item)` 合并相邻空闲块。


`varmem_plug_holes(item)` 做两步：

1. 向后合并：后继是 FREE 且不是 `heap_end` 时，吞并后继块。
2. 向前合并：前驱是 FREE 时，让前驱吞并当前块。

所以它能处理：

- 中间洞保持不变（前后都是 USED 时无法合并）。
- 尾部释放后与尾部 FREE 块合并。
- 相邻 FREE 块逐步并成更大连续块。

## 6. 与 lfree 版本的关键差异

`varmem.c` 是“无 `lfree`”版本，和优化版 small memory 的关键差异：

- 扫描起点固定是 `heap_ptr`，不是最低空闲块。
- 块头只用 `used/next/prev`，没有把状态塞进 pool 指针最低位。
- 代码更直观，便于教学；但扫描效率通常低于带 `lfree` 的实现。

## 7. 建议的观察命令

### 7.1 快速看完整流程

```text
varmem demo
```

`demo` 会自动执行：

- 初始化。
- 连续分配 `16/32/64/16`。
- 依次释放 `p2`、`p4`、`p3`。
- 每一步打印 `trace` 观察块链表变化。

### 7.2 手动操作

```text
varmem init
varmem alloc 16
varmem alloc 32
varmem trace
varmem free <ptr>
varmem info
```

`trace` 输出里重点看：

- `off/hdr/user/size/USED|FREE/next/prev`。
- `search policy : first-fit from heap head, no lfree`。
- `used` 与 `max_used` 的变化是否符合预期。
