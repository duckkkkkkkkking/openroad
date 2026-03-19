# `improve_placement` 崩溃分析与修复说明

## 1. 问题概述

`impl.sh` 在执行 `improve_placement` 时，会在部分 testcase 上崩溃。这个问题不是随机性内存踩踏，也不是后续优化 pass 的偶发错误，而是一个稳定可复现的“数据假设不成立”问题：

- detailed placement 的实现默认认为，所有进入 row-based legalization / orientation / alignment check 的实例都具有合法的 row `SITE`
- 但部分 testcase 中存在 `PLACED` 的 SRAM 宏块，这些宏块的 LEF 是 `CLASS BLOCK`，没有 `SITE`
- 旧代码仍然把这类实例当成 movable multi-height cell 处理，最终在读取 row site / orientation 映射时崩溃

这次修复分两阶段完成：

1. 第一阶段：把无 `SITE` 的实例从 row-based detailed placement 的 movable cell 集合中剔除，并作为固定障碍物处理，先消除主 crash
2. 第二阶段：把同一条过滤条件补到 orientation / alignment / grid repaint 等后续路径，去掉残余 warning，并加上防御式保护

## 2. 复现现象

已确认的原始失败 case：

- `mempool_tile_wrap`
- `NV_NVDLA_partition_c`

已做回归验证且正常的 case：

- `des3`
- `fpu`
- `mc_top`
- `netcard_fast`
- `tv80s`

两个失败 case 的共同点：

- `mempool_tile_wrap` 中有 20 个 `PLACED` SRAM 宏块
- `NV_NVDLA_partition_c` 中有 128 个 `PLACED` SRAM 宏块
- 它们都不是 `FIXED`，因此旧逻辑会把它们当作“可参与 detailed placement 的实例”

原始崩溃前的关键日志分别为：

### `mempool_tile_wrap`

```text
[INFO DPL-0318] Collected 182920 single height cells.
[INFO DPL-0319] Collected 20 multi-height cells spanning 62 rows.
[INFO DPL-0310] Assigned 182920 cells into segments.  Movement in X-direction is 0.000000, movement in Y-direction is 0.000000.
Error: place.tcl, 44 bad optional access
```

### `NV_NVDLA_partition_c`

```text
[INFO DPL-0318] Collected 167705 single height cells.
[INFO DPL-0319] Collected 128 multi-height cells spanning 124 rows.
[INFO DPL-0310] Assigned 167705 cells into segments.  Movement in X-direction is 0.000000, movement in Y-direction is 0.000000.
Error: place.tcl, 44 bad optional access
```

这两条日志说明：

- single-height standard cell 已经正常完成 segment assignment
- 程序在开始处理“被识别成 multi-height cell”的那批实例时崩溃

## 3. 源码级根因分析

### 3.1 调用链

与本问题直接相关的调用链如下：

1. `Opendp::improvePlacement(...)`
2. `ShiftLegalizer::legalize(...)`
3. `DetailedMgr::collectFixedCells()`
4. `DetailedMgr::collectSingleHeightCells()`
5. `DetailedMgr::collectMultiHeightCells()`
6. `DetailedMgr::assignCellsToSegments(...)`
7. `DetailedMgr::paintInGrid(...)`

也就是说，崩溃发生在真正的 MIS / global swap / vertical swap / reorder / random improver 之前，属于 legalization 阶段的数据分类问题。

### 3.2 为什么 SRAM 宏块会被误识别为 multi-height cell

`Architecture::getCellHeightInRows(const Node*)` 的逻辑本质上是：

```cpp
round(cell_height / row_height)
```

失败 case 中的 SRAM 宏块高度与 row 高度关系如下：

- standard row 高度：`0.270`
- SRAM 高度：`16.800` 或 `33.600`

于是得到：

- `16.800 / 0.270 = 62.22...`，四舍五入后是 `62`
- `33.600 / 0.270 = 124.44...`，四舍五入后是 `124`

这与日志中的：

- `20 multi-height cells spanning 62 rows`
- `128 multi-height cells spanning 124 rows`

完全一致，说明日志里所谓的 multi-height cell，实际就是这些 SRAM `BLOCK` 宏块。

### 3.3 为什么这些实例根本不应该进入 row-based detailed placement

这些 SRAM 宏块的 LEF 具有如下特征：

- `CLASS BLOCK`
- 没有 `SITE`

而 detailed placement 的 row/pixel 结构只记录 DEF row 对应的 row site 和 orientation，典型是标准单元 row 的 `asap7sc7p5t`。这意味着：

- row-based legalization 只能处理“和 row site 兼容”的实例
- 对于没有 `SITE` 的 `BLOCK` 宏块，row site/orientation 根本没有定义

因此，“把无 `SITE` 的宏块当成 movable row cell”从数据模型上就是错误的。

### 3.4 为什么会崩溃

我最初在 `/OpenROAD` 上分析时，看到的实现是通过：

```cpp
grid_->getSiteOrientation(...).value();
```

直接解包空 `std::optional`，所以报错是 `bad optional access`。

而在本次实际修复的仓库 `/opt/contest/openroad` 中，对应代码路径是：

```cpp
pixel->sites.at(node->getDbInst()->getMaster()->getSite())
```

虽然实现形式不同，但假设完全相同：

- `master->getSite()` 必须非空
- `pixel->sites` 里必须存在这个 `SITE`

对无 `SITE` 的 SRAM 宏块，上述前提不成立，所以程序沿着同一类错误假设崩溃。

### 3.5 为什么 `valgrind` 不是本问题的主要证据来源

这个问题本质上不是非法内存读写，而是“对无效逻辑前提进行解包/查表”。因此：

- `gdb` / 调用栈更容易直接定位责任函数
- `valgrind` 即使运行，也不会像典型野指针问题那样给出更强的主证据

也就是说，这里更关键的是源码阅读和调用链分析，而不是内存检查工具本身。

## 4. 第一阶段修复

第一阶段的目标是先切断主 crash 路径，修复方向是：

> 无 `SITE` 的实例不能参与 row-based detailed placement，只能作为固定障碍物存在。

### 4.1 修改位置

文件：`src/dpl/src/optimization/detailed_manager.cxx`

### 4.2 修改内容

1. 增加 `usesRowSites(const Node*)`
   - 通过 `node->getSite() != nullptr` 判断实例是否具有 row `SITE`

2. 修改 `collectSingleHeightCells()`
   - 旧逻辑：只跳过 `terminal` / `fixed`
   - 新逻辑：额外跳过无 `SITE` 的实例

3. 修改 `collectMultiHeightCells()`
   - 旧逻辑：只跳过 `terminal` / `fixed` / single-height
   - 新逻辑：额外跳过无 `SITE` 的实例

4. 修改 `collectFixedCells()`
   - 旧逻辑：只把 `fixed` 实例当作 blockage
   - 新逻辑：把无 `SITE` 的非 terminal 实例也当作固定障碍物收集进去

### 4.3 第一阶段修复效果

第一阶段完成后：

- `mempool_tile_wrap` 不再崩溃
- `NV_NVDLA_partition_c` 不再崩溃
- 这些 SRAM 宏块不再被当成 movable multi-height cell
- 但后续仍会出现 row alignment / orient warning

原因是：

- 虽然它们已经不进入 legalization 的 movable 集合
- 但旧代码在 `checkRowAlignment()`、`checkSiteAlignment()`、`DetailedOrient::orientCells()` 等后续路径里，仍然按“非 fixed、非 terminal”扫描全网表
- 这会把同一批无 `SITE` 宏块再次算进去，只是不再 crash，而是产生 warning

## 5. 第二阶段修复

第二阶段的目标是把第一阶段的设计边界补完整：

> 只要某个 pass 是 row-based 的，就不应该再处理无 `SITE` 的实例。

### 5.1 修改总览

| 文件 | 函数 | 修改目的 |
| --- | --- | --- |
| `src/dpl/src/optimization/detailed_manager.cxx` | `collectSingleHeightCells` | 无 `SITE` 实例不进入 single-height movable 集合 |
| `src/dpl/src/optimization/detailed_manager.cxx` | `collectMultiHeightCells` | 无 `SITE` 实例不进入 multi-height movable 集合 |
| `src/dpl/src/optimization/detailed_manager.cxx` | `collectFixedCells` | 无 `SITE` 实例作为固定障碍物收集 |
| `src/dpl/src/optimization/detailed_manager.cxx` | `checkSiteAlignment` | row-based 检查跳过无 `SITE` 实例 |
| `src/dpl/src/optimization/detailed_manager.cxx` | `checkRowAlignment` | row-based 检查跳过无 `SITE` 实例 |
| `src/dpl/src/optimization/detailed_manager.cxx` | `paintInGrid` | 仅在 `SITE` 存在且 pixel 中有对应映射时同步朝向 |
| `src/dpl/src/optimization/detailed_orient.cxx` | `orientCells` | row orientation pass 跳过无 `SITE` 实例 |
| `src/dpl/src/util/journal.cxx` | `paintInGrid` | undo/redo 重绘路径加入同样的空 `SITE` 防护 |
| `src/dpl/src/infrastructure/Grid.cpp` | `gridHeight(dbMaster*)` | 非 uniform-row 路径下，空 `SITE` 回退到单行高度语义 |
| `src/dpl/src/infrastructure/Grid.cpp` | `gridHeight(const Node*)` | 同上，避免再次假设 `SITE` 必然存在 |

### 5.2 第二阶段的核心思想

第二阶段不是引入新的 placement 策略，而是把第一阶段已经确定的语义贯彻到底：

- 无 `SITE` 的宏块仍然存在于设计中
- 它们仍然通过 blockage 影响 standard cell 可放置区域
- 但它们不再进入任何 row-based 的 legalization / orientation / alignment 流程

这是一种保守而正确的修复方式，因为 detailed placement 的 row 算法本来就不是为 `CLASS BLOCK` 无 `SITE` 宏设计的。

### 5.3 第二阶段修复效果

第二阶段完成后：

- 原先的 crash 已继续保持消失
- `DPL-0381` 的 orient warning 消失
- `Found N row alignment problems` 消失
- `Found N site alignment problems` 维持为 0
- grid repaint / journal undo / redo / 非 uniform-row 辅助路径也不再假设 `SITE` 必然存在

## 6. 编译与验证

### 6.1 编译

使用以下命令重新编译：

```bash
cmake --build /opt/contest/openroad/build -j 8 --target openroad
```

### 6.2 验证命令

```bash
OPENROAD_BIN=/opt/contest/openroad/build/src/openroad DESIGNS=mempool_tile_wrap /opt/contest/impl.sh
OPENROAD_BIN=/opt/contest/openroad/build/src/openroad DESIGNS=NV_NVDLA_partition_c /opt/contest/impl.sh
OPENROAD_BIN=/opt/contest/openroad/build/src/openroad DESIGNS=des3 /opt/contest/impl.sh
```

### 6.3 验证结果

#### `mempool_tile_wrap`

- 退出码：`0`
- `bad optional access`：未出现
- `DPL-0381`：未出现
- `Found 0 row alignment problems`
- `Found 0 site alignment problems`
- 最终结果：

```text
Detailed Improvement Results
Original HPWL          1033599.4 u
Final HPWL              980442.5 u
Delta HPWL                  -5.1 %
```

#### `NV_NVDLA_partition_c`

- 退出码：`0`
- `bad optional access`：未出现
- `DPL-0381`：未出现
- `Found 0 row alignment problems`
- `Found 0 site alignment problems`
- 最终结果：

```text
Detailed Improvement Results
Original HPWL          1876037.0 u
Final HPWL             1805947.0 u
Delta HPWL                  -3.7 %
```

#### `des3`

- 退出码：`0`
- 未观察到回归
- `Found 0 row alignment problems`
- `Found 0 site alignment problems`
- 最终结果：

```text
Detailed Improvement Results
Original HPWL             7752.7 u
Final HPWL                7233.7 u
Delta HPWL                  -6.7 %
```

## 7. 最终结论

这次问题的根因可以归纳为一句话：

> `improve_placement` 的 row-based detailed placement 代码错误地把“没有 row SITE 的 BLOCK 宏块”当成了可移动 row cell。

正确的处理方式不是强行让这些宏参与 row legalization，而是：

- 不把它们加入 movable single/multi-height cell 集合
- 把它们保留为固定障碍物
- 在所有 row-based check / orient / repaint 路径中都跳过或防御性处理这类实例

修复后，两个原始崩溃 case 都可以完整跑通，且第二阶段已经把相关 warning 一并收口。该修复保持了行为上的保守性，不会错误地尝试移动本就不属于 row-based detailed placement 域的 SRAM `BLOCK` 宏。
