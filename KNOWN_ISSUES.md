# 已知问题（忠实移植阶段保留）

本库第一版的目标是**把 pico-hid-mapper 里验证过的解析行为逐位搬过来**，
因此下列问题**故意保留**，并用主机侧样本测试把现有行为固定住。
修它们会改变解析结果，所以单独一轮处理（修的时候这些用例会红，届时按新行为更新）。

## 1. 报告描述符解析：usage range 的循环上界用错（影响最大）

`hid_parser.c` 的通用字段解析里，处理 `Usage Minimum/Maximum`（`0x19/0x29`）时
循环上界用的是 range 长度，而没有与 `Report Count`（`0x95`）取较小值：

```c
for (j = 0; j < range; j++) { ... bit_offset += report_size; }
/* 应为 min(count, range) */
```

当 `count < range` 时 `bit_offset` 会被多推进 → **该字段之后的所有字段偏移全部错位**，
表现为鼠标轴/按键读到错误字节、NKRO 段解析失败等。

- 触发条件：描述符里 range 个数多于该字段的 report count（部分厂商键盘/复合设备会这样）。
- 现状：样本测试没有覆盖这种描述符（只覆盖标准鼠标与标准 NKRO 位图段）。
- 修复方向：`min(count, range)`，并补一条"range 多于 count"的样本用例。

## 2. 报告描述符解析：usage 列表不足时环绕复用

同一函数里局部 usage 列表（`0x09` 多次出现）的取值是 `loc_usages[j % loc_usage_count]`，
即数量不足时**从头环绕**。按 HID 规范应为"重复最后一个 usage"。
NKRO 那条独立的解析路径实现是对的 —— 两条路径语义不一致。

- 影响：usage 列表比 report count 少时字段 usage 归属错误（可能把某个轴认成按键）。
- 修复方向：改为钳到最后一个下标，并与 NKRO 路径统一。

## 3. 只支持 1 字节形式的 Global item

`Usage Page(0x05)` / `Report Size(0x75)` / `Report ID(0x85)` / `Report Count(0x95)`
只处理 1 字节形式；2 字节形式（`0x06/0x76/0x86/0x96`）落进 `default` 被**跳过但不更新状态**，
于是**沿用上一个 item 的旧值**，而不是记录新值。

- 影响：带 2 字节 Usage Page（如 `0x06 0x00 0xFF` 厂商页）的描述符解析结果错误。
- 修复方向：支持 2 字节形式；不支持的类型也应按规范**重置**对应全局状态。

## 4. `0xC0 End Collection` 不清 local 状态

规范要求 End Collection 清空局部 usage/usage-min/max 状态，当前实现不清。

- 影响：跨 collection 的字段可能继承上一个 collection 的 usage，导致归属错误。
- 修复方向：`0xC0` 分支里清 local 状态。

## 5. 多 Report ID 描述符只保留最后一个

`hid_mouse_desc_t.report_id` 只记最后一个 `Report ID`，多 report ID 的描述符信息丢失
（NKRO 那条路径按段记录 ID，是对的）。

- 影响：多 report ID 鼠标（复合设备）只有最后一个 ID 的报文能对上。
- 修复方向：鼠标侧也按"每个字段记录自己的 report ID"来做（对齐 NKRO 的实现）。

---

## 另外两处不是缺陷、但值得知道

- **固定 8 字节鼠标路径**（`hid_dispatch_mouse`）只接受 `len == 8`；这是设备侧固定
  格式的约定，宿主侧鼠标请走描述符路径（`hid_mouse_parse` + `hid_mouse_dispatch`）。
- **鼠标无描述符时静默丢弃**：`proto == MOUSE` 但 `report_desc == NULL`（描述符超过宿主
  枚举缓冲）时，本库仍会接管该设备，但不产生事件。宿主可用
  `hidkit_mount()` 的返回值与 `hidkit_is_active()` 自行决定要不要走兜底格式。
