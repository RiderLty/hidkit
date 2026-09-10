# 已知问题

本库第一版的目标是**把 pico-hid-mapper 里验证过的解析行为逐位搬过来**，所以当时
**故意保留**了一批描述符解析缺陷，并用主机侧样本测试把旧行为固定住。
这批缺陷（5 条）现在已经修完，逐条记在下面「已修复」一节；每条修复都补了样本用例
（见 `tests/host/test_hidkit.c` 末节「缺陷修复样本」），用例按规范行为断言，
**在修复前的实现上会红**（实测：6 个新用例共 25 条断言失败）。

## 已修复

### 1. usage range 的循环上界用错（原影响最大）

`Usage Minimum/Maximum`（`0x19/0x29`）展开字段时，循环上界原来直接用 range 长度，
没有与 `Report Count`（`0x95`）取较小值 → `count < range` 时 `bit_offset` 多推进，
**该字段之后的所有字段偏移全部错位**。

- 修法：`src/hid_parser.c:218`（`0x81 Input` 分支），上界改为 `min(count, range)`；
  `count > range` 时多出的位仍计入 `bit_offset`，但视作 padding 不产生字段 ——
  与 NKRO 路径「超出范围即 break」的语义一致。NKRO 路径本来就对，未动。
- 用例：`k_desc_range_gt_count`（range=4 而 count=2）。修复前按键段被展开成 4 个字段、
  其后 X/Y 的位偏移是 10/18；修复后是 2 个字段 + 8/16。运行时报文位移也从读错字节的
  `(-63,0)` 变成正确的 `(5,-5)`。

### 2. usage 列表不足时环绕复用

局部 usage 列表（多个 `0x09`）不够 `Report Count` 用时，原来取
`loc_usages[j % loc_usage_count]`，即**从头环绕**；规范要求**重复最后一个**。

- 修法：`src/hid_parser.c:199`，改为钳到最后一个下标
  （`j < count ? j : loc_usage_count - 1`），与 NKRO 路径的写法一致。
- 用例：`k_desc_usage_short`（usage = X、Y，Report Count = 4）→ 第 3/4 个字段的
  usage 必须是 Y(0x31)；修复前会绕回 X(0x30)，把后续字段的 usage 归属搞错。

### 3. 只支持 1 字节形式的 Global item

`Usage Page(0x05)` / `Report Size(0x75)` / `Report ID(0x85)` / `Report Count(0x95)`
原来只处理 1 字节形式，2 字节形式（`0x06/0x76/0x86/0x96`）落进 `default`：
**跳过但也不更新状态**，于是沿用上一个 item 的旧值。

- 修法：通用解析器 `src/hid_parser.c:101`–`155`、NKRO 路径
  `src/hid_parser.c:507`–`549` 都改成用 `read_unsigned()` 按 item 自带宽度取值，
  1B/2B 形式通吃；4 字节形式（`0x07/0x77/0x87/0x97`）确实不支持，则按"不沿用旧值"
  把对应全局状态**复位**（Usage Page→0、Report Size→0、Report Count→0、
  Report ID→无 ID）。2 字节 Report ID 的高字节非零（非法值）同样按"无 ID"处理。
- 用例：`k_desc_two_byte_globals`（2B Usage Page + 2B Report Count + 4B Report Count
  复位）、`k_desc_two_byte_report_id`（2B Report ID = 9）、NKRO 侧的
  `k_nkro_desc_two_byte_page`（2B Usage Page 声明 Keyboard 页 —— 修复前整条描述符
  识别不出键盘，`hid_nkro_parse` 直接返回 false）。
- 仍未实现：`Push(0xA4)` / `Pop(0xB4)` 全局状态栈（见文末）。

### 4. `0xC0 End Collection` 不清 local 状态

规范里 Collection / End Collection 都是 Main item，都要复位局部状态；原来 `0xC0`
只递减嵌套计数，于是集合结束后新开的 Input 会**继承上一个集合的 usage**。

- 修法：通用解析器 `src/hid_parser.c:253`、NKRO 路径 `src/hid_parser.c:648`
  的 `0xC0` 分支清 `loc_usage_count` / `loc_has_usage_range`（与 `0xA1` 分支一致）。
- 用例：`k_desc_end_collection`（集合内留一个没配 Main item 的 `Usage (Wheel)`）→
  集合外那个"无 usage"的 Input 必须不产生字段；修复前它继承 Wheel，
  字段数 2 且 `idx_wheel` 被命中。

### 5. 多 Report ID 描述符只保留最后一个

`hid_mouse_desc_t.report_id` 原来只记最后一个 `Report ID`，多 report ID 描述符里
只有最后一个 ID 的报文能对上（NKRO 路径按段记 ID，本来就是对的）。

- 修法：按字段记录所属 ID，而不是按描述符记一个：
  - `hid_field_t` 增加 `report_id`（`src/hid_parser.h:85`），
    `hid_parse_report_fields()` 在展开字段时落上当前 Report ID；
  - `hid_mouse_desc_t.report_id` 语义收敛为「描述符主 ID = 首个非零 ID」
    （0 = 整份描述符无 ID），仅供宿主参考；
  - `hid_mouse_dispatch()` 改为逐字段用 `mouse_field_body()`
    （`src/hid_parser.c:376`）按字段自己的 ID 定位报文体：ID 不符的字段本次报文
    不参与更新 —— 轴字段读 0（不产生位移），按键字段**保持上次状态**
    （不会被别的 Report ID 的报文误判成"松开"）；
  - 按键索引由「起始字段 + 个数」改成下标表 `btn_idx[]`
    （`src/hid_parser.h:107`）：多 ID 描述符里按键可能不与轴连续排列，
    按起始下标顺序取会连带把轴字段当成按键读。
- 用例：`k_desc_multi_report_id`（ID=1 = 按键 + X/Y，ID=2 = 滚轮）→ 两个 Report
  都要能解析；修复前 ID=1 的报文被整份丢弃（`report_id` 只留了 2）。
- 仍然存在的限制：某个 usage 在多个 Report ID 里各定义一份时，
  `idx_x/idx_y/idx_wheel` 只指向**第一个**匹配字段，其余 Report 的该轴不解析
  （丢事件，不会读错值）；鼠标按键上限仍是 8（`uint8_t` 位掩码宽度，
  `HID_MOUSE_MAX_BTNS`），与修复前一致。

---

## 另外几处不是缺陷、但值得知道

- **固定 8 字节鼠标路径**（`hid_dispatch_mouse`）只接受 `len == 8`；这是设备侧固定
  格式的约定，宿主侧鼠标请走描述符路径（`hid_mouse_parse` + `hid_mouse_dispatch`）。
- **鼠标无描述符时静默丢弃**：`proto == MOUSE` 但 `report_desc == NULL`（描述符超过宿主
  枚举缓冲）时，本库仍会接管该设备，但不产生事件。宿主可用
  `hidkit_mount()` 的返回值与 `hidkit_is_active()` 自行决定要不要走兜底格式。
- **`Push(0xA4)` / `Pop(0xB4)` 未实现**：这两个 item 目前只被跳过，不做全局状态压栈/
  恢复（罕用；带 Push/Pop 的描述符里，Push 之后的全局改动不会在 Pop 处还原）。
- **描述符容量**：通用解析器最多 `HID_PARSE_MAX_FIELDS`(64) 个字段、鼠标
  `HID_MOUSE_MAX_FIELDS`(24) 个字段，字段数触顶后 `bit_offset` 停止累加（后续字段
  偏移会错）——超长的第三方描述符仍可能踩到，与移植前的行为一致。
