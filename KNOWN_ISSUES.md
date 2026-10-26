# 已知问题

本库第一版的目标是**把一套经过实际设备验证的解析行为逐位搬过来**，所以当时
**故意保留**了一批描述符解析缺陷，并用主机侧样本测试把旧行为固定住。
这批缺陷（1–5 条）现在已经修完，逐条记在下面「已修复」一节；每条修复都补了样本用例
（见 `tests/host/test_hidkit.c` 末节「缺陷修复样本」），用例按规范行为断言，
**在修复前的实现上会红**（实测：6 个新用例共 25 条断言失败）。

移植完成之后又修了两条（6、7）：第 6 条是与上面**同类**的规范符合性问题（既有
实现里就是这样），第 7 条是**移植过程本身引入的回归**（把 `instance` 改成
`slot` 时加的守界误伤了探测调用），真机上表现为「手柄全部不认识」。两条同样按
「先加用例、确认在修复前会红」的流程做。

后来在一台「不被支持的全键无冲键盘」上（Compx 2.4G 接收器 `3554:fa09`：
2 个 HID 接口，第二个接口谎报 `proto=Mouse`）又定位并修了两条（8、9）：
第 8 条是**设备类型判定只看 `bInterfaceProtocol`**，第 9 条是
**NKRO 数组段的松开漏检**（第 8 条把 boot 风格键盘改走描述符路径后暴露出来的
既有缺陷）。

最近的现场反馈（没碰鼠标但光标一直飞、鼠标按键被识别成键盘按键）又定位到一条
（11）：**报文长度完全没校验**。描述符声明的是多少位，报文就应该是多少字节；
不校验的话 2 字节杂报文会被当成 11 字节鼠标报文解出天文位移，同接口上的鼠标报文
也会被 boot 键盘解码器读成按键。修法是把描述符声明的长度变成硬判据
（对齐 Linux `hid_report_raw_event()` 的 "was too short" 拒收；内核新旧版本
在此分别返回 -EINVAL / 补零，本库只取前者）。

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
- `Push(0xA4)` / `Pop(0xB4)` 在后续「解析增强」里补上了（此前只跳过）。

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
- 后续增强（见文末「解析增强」）：`hid_mouse_dispatch()` 改为**逐字段扫描**后，
  同一 usage 在多个 Report ID 里各定义一份的情况已覆盖（每个字段按自己的
  Report ID 取数）；`idx_x/idx_y/idx_wheel` 退化为查询用的首个命中下标。
  鼠标按键上限仍是 8（`uint8_t` 位掩码宽度，`HID_MOUSE_MAX_BTNS`）。

---

### 6. `Report Count = 0` 的 main item 被当成 1 个字段

`Report Count`（`0x95`）为 0 时，按规范该 main item **不产生任何字段、也不占位**
（全局量默认值就是 0，"Report Count 0 + Input" 是合法的空操作写法，描述符里用来
占位/占坑）。两处解析路径都把它改成 1 再用：

```c
uint8_t count = glb_report_count;
if (count == 0) count = 1;                                  /* 通用字段解析 */
uint16_t count = glb_report_count ? glb_report_count : 1;   /* NKRO 路径 */
```

于是它白推进了 `Report Size` 位 —— 与第 1 条**同一失效模式**：其后的字段偏移
全部错位（实测：X/Y 前放一个 `Report Size 8 / Report Count 0` 的空操作项，Wheel
被从 bit 16 推到 bit 24，跨出报文长度、事件直接丢失）。

- 修法：`src/hid_parser.c` 两处都改为直接取 `glb_report_count`（0 即 0）——
  `0x81 Input` 分支的四个出口（常量、usage 列表、usage 范围、无 usage）在
  `count == 0` 时本来就都推进 0 位，只需去掉那个替换。
- 用例：`k_desc_zero_report_count`（鼠标路径：Wheel 必须仍在 bit 16、报文要解析出
  `wheel=1`）与 `k_nkro_desc_zero_report_count`（NKRO 路径：键位段偏移必须是 0）。
  把旧行为放回去，这两条共 4 条断言立刻复红。
- 连带更新：第 3 条的用例（`k_desc_two_byte_globals`）末尾那个 Input 原本断言
  "Report Count 复位后按 1 个字段算" = 3 个字段 —— 那是旧行为的产物。按规范，
  4 字节 `Report Count`（不支持 → 复位为 0）之后该 Input 不产生字段，故期望改为
  2 个字段（X/Y）、`idx_wheel == 0xFF`；这个判据比原来更能区分"复位为 0"与
  "留着旧值 5"（后者会展开 7 个字段）。

### 7. 移植引入的回归：HID 手柄布局表恒不命中（真机上"手柄全不认识"）

真机现象（DualSense，VID:PID `054c:0ce6`，描述符已抓到）：

```
[HKDBG]  hidkit: unhandled 054c:0ce6 proto=0 desc=yes
```

根因不在布局表，而在**探测调用被守界挡掉**：`hidkit_mount()` 判定"这是不是已知
手柄"时用的是

```c
gamepad_hid_mount(-1, dev->vid, dev->pid, ...)   /* -1 = 只探测，不占槽位 */
```

而 `gamepad_hid_mount()` 入口有 `slot < 0` 守界（移植时按"`instance`(uint8_t) →
`slot`(int8_t) 并在入口守界"这条规则统一加的）→ 探测**恒**返回 false → DS5/Azeron
等所有已知手柄都落到 NKRO 分支、最终返回未消费。原实现里 `instance` 无符号，
这个守界无害；改成有符号的 `slot` 后，"拿 -1 当探测"这个隐含约定就被破坏了。

- 修法：把"匹配"与"挂载"拆开 —— 新增 `gamepad_hid_match(vid, pid)`（纯 VID/PID
  查表，不碰槽位状态），`gp_lookup()` 收拢匹配规则供两者共用（新增手柄仍只改一处）；
  `hidkit_mount()` 改用 `gamepad_hid_match()` 探测，确认接管后才分配槽位、再
  `gamepad_hid_mount(slot, ...)`。
- 用例：`test_hid_gamepad_ds5`（挂载 `054c:0ce6` → 断言认领、`hidkit_is_gamepad()`、
  报文解析出 Cross 按下/松开与轴值）。**修复前 8 条断言失败**。
- 为什么一直没被发现：样本测试里没有任何用例走"按 VID/PID 认领 HID 手柄"这条路径
  （`test_xinput` 只测归一化入口，注释里写着布局表"由真机回归覆盖"），于是这条
  路径只在真机上暴露。

### 8. 设备类型只看 `bInterfaceProtocol`，复合接口的键盘集合被丢给鼠标解析器

真机现象（Compx 2.4G 接收器 `3554:fa09`）：按 1..5 有键事件，第 6 键起（含 7 8 9 0）
全部无输出。抓包显示该设备有两个接口：

| 接口 | 描述符声明 | 实际集合 | 旧判定 |
|---|---|---|---|
| 0 | proto=Keyboard | 修饰键位图 + 5 槽键码数组 | boot 键盘，正常 |
| 1 | **proto=Mouse** | 厂商 + Consumer + System + **Keyboard(ID 4, 160 bit NKRO)** + Mouse(ID 7) | 鼠标，键盘集合被丢 |

旧 `hidkit_mount()` 先看 proto：`MOUSE → KIND_MOUSE`、`KEYBOARD → KIND_KB`，
只有 `proto == 0` 才尝试 `hid_nkro_parse()`。于是接口 1 被挂成 `SLOT_MOUSE`，
ID=4 的键盘报文进 `hid_mouse_dispatch()` 后不匹配任何鼠标字段 → 静默丢弃
（`hidkit_report()` 仍返回 true，宿主的兜底 hexdump 也不会触发）。1..5 之所以正常，
是因为它们走接口 0 的 boot 报表。

- 修法：判定改为**描述符优先**。`hidkit_mount()` 只要拿到描述符就解析出**能力集**
  （`CAP_KB_NKRO` / `CAP_MOUSE_DESC` / `CAP_GAMEPAD`），槽位保存能力位；
  `hidkit_report()` 按报文首字节（Report ID）路由，键盘优先、其次鼠标
  （第 11 条之后改为按描述符声明的报文长度择优）。
  `hid_nkro_accepts()` / `hid_mouse_accepts()` 是新增的路由判据。
  proto 只在描述符缺失、或描述符里没有本库认识的集合时兜底。
  已知手柄的 VID:PID 匹配仍排在描述符解析之前（手柄描述符里也有 Generic Desktop
  X/Y + Button，否则会被误判成鼠标）；鼠标能力的认领条件是 `proto == MOUSE`
  **或**描述符里有 Mouse/Pointer 顶层集合（`hid_desc_has_mouse_collection()`），
  后者要求确实解析出鼠标字段 —— 这样「键盘 + 鼠标」复合接口即使只写
  `proto=Keyboard/0` 也能拿到鼠标集合，而 Gamepad 集合不会被误判成鼠标。
- 顺带修掉的相关问题：`hid_mouse_parse()` 现在用字段过滤（只收 Generic Desktop /
  Button 页）**不占字段槽地跳过**厂商/键盘集合。该接口原先把 19 个厂商字段 +
  Consumer/System 字段塞满 `HID_MOUSE_MAX_FIELDS(24)`，后面的鼠标集合（ID 7）
  根本轮不到，`idx_x/y/wheel` 全是 `0xFF` —— 即这个接口**当鼠标也是坏的**。
  过滤后 `hid_mouse_parse()` 正确得到 X@8 / Y@24 / Wheel@40（ID 7 内偏移）。
- 用例：`test_combo_mouse_nkro()`（`3554:fa09` 的 218 字节描述符 + 真机报文）
  → 同一槽位上 ID=4 出键事件、ID=7 出鼠标事件、未解析的 Consumer 报文返回「未消费」；
  `test_combo_kb_mouse_single_itf()`（`proto=Keyboard` + 键盘 ID=1 / 鼠标 ID=2）
  → 键鼠都出事件，且 Gamepad 集合的描述符仍返回未消费。

### 9. NKRO 数组段的松开漏检
数组段（`Report Size 8` 的 keycode 数组，6KRO / boot 风格描述符）的报文只列出
**当前按着的键**，松开的键不再出现。旧 `hid_nkro_dispatch()` 的数组分支只对本帧
出现的 keycode 调 `nkro_key_edge()`：

```c
uint8_t kc = (uint8_t)hid_field_read(&f, body, body_len);
nkro_key_edge(slot, dev, kc, kc != 0);   /* 消失的键不会被处理 */
```

于是按下 1..5 后松开 2，新报文里不再有 usage 0x1F，shadow 里该位却永远留着 ——
上层再也等不到这支键的松开。位图段没这个问题（每个 bit 都显式给出 up/down）。

这条缺陷在旧实现里被第 8 条掩盖着：boot 风格键盘当时走 `hid_dispatch_keyboard()`
（集合比较，松开正确）。第 8 条改成描述符优先后，boot 风格描述符会走 NKRO 路径，
缺陷立刻暴露（真机 itf0 松开 2~5 时无事件）。

- 修法：数组段先收集本帧键码集合（`hid_nkro_dev_t.array_prev`，256 位位图），
  再与上一帧集合做差：`now && !before` → 按下，`!now && before` → 松开。
  位图段仍逐位即时处理；两段共用一个 shadow，重复键仍天然去重。
- 用例：`test_nkro_array_release()`（真机 itf0 描述符：位图修饰键 + 5 槽数组）
  → 松开 2~5 必须补发 4 个松开，且 Shift（位图段）不被数组去重误释放。

### 10. 描述符鼠标卸载不补发按住的按键（真机 gadget 验证发现）

`slot_release()` 原来只调 `hid_nkro_release_all()`（键盘）与 `hid_dispatch_reset()`
（boot 键鼠）。但**描述符鼠标**的按键状态在 `s_mouse[].last_buttons`，既不在
NKRO shadow 里、也不在 `hid_dispatch` 的 slot 状态里 —— 于是"按着鼠标左键拔掉
设备/卸载"时，上层永远等不到松开（映射引擎里该键会卡住）。

主机样本测试没覆盖到，是在树莓派上用 USB gadget 真机验证时，从事件计数
（按着左键卸载后 `key` 计数少 1）发现的。

- 修法：新增 `hid_mouse_release_all()`（`src/hid_parser.c`），`slot_release()`
  在 `caps & CAP_MOUSE_DESC` 时调用；bit i ↔ `btn_idx[i]`，与 dispatch 编码一致。
- 用例：`test_mouse_umount_release()`（样本），以及真机 gadget 的
  `tests/linux/verify_gadget.py` case A/B（断言卸载后的 `MOUSE ... up`）。

### 11. 报文长度不校验：2 字节杂报文被当成 11 字节鼠标报文（真机"光标乱飞"）

现场现象（`QIN_LOG/加载日志2`）：**没碰鼠标，鼠标信号每秒 187 次持续输出**，
`STATUS fps≈3700`。HIDRAW 抓到的报文全是 2 字节：

```
conn=2 addr=2 itf=1 rid=00 id=0 len=2 trunc=0 data=BE EF
```

该接口（ROG OMNI RECEIVER `0b05:1ace` itf=1）的描述符是无 Report ID 的 **11 字节**
鼠标：5 个按键位 + 3 位补齐 + **16 位 X / 16 位 Y** + 8 位滚轮 + 5 字节厂商段。
`hid_field_read()` 只有**字段级**越界检查（`end_byte > report_len` 才返回 0）：
X 字段占 bit 8..23，在 2 字节报文里"读得到" → `0xEFBE` 符号扩展成 **-4162**，
Y 同理 → 每帧一个天文位移。

- 根因：把"字段读得到"当成了"报文是给我的"。描述符声明的报文长度才是权威判据
  —— Linux `hid_report_raw_event()` 正是这样拒收 `report N was too short` 的报文。
- 修法：
  - 新增 `hid_report_len_t` + `hid_desc_report_bits()`（`src/hid_parser.c`）：
    解析描述符时为**每个 Report ID** 累计 Input item 的位宽（Output/Feature 不计），
    并按最外层 Application Collection 归类成键盘/鼠标/其它；
    `hid_mouse_desc_t` / `hid_nkro_desc_t` 各存一张表（`HID_REPORT_LEN_TABLE = 8`）。
  - `hid_mouse_accepts()` / `hid_nkro_accepts()` 改为 `hid_mouse_match()` /
    `hid_nkro_match()`（返回 0/1/2 = 不属于/长度够/长度精确吻合）：
    **无 Report ID 的描述符不再无条件接受**，报文必须装得下描述符声明的总位宽。
  - `hidkit_report()` 的路由去掉"键盘优先"：两份解析器同时命中时按匹配强度择优，
    强度相同（不用 Report ID 的描述符 = 键鼠字段在同一份报文里）则**并行**处理；
    没有 Report ID 且长度判据也无法区分的报文不再猜成键盘。
  - `CAP_KB_BOOT` 兜底不再无条件解码：描述符可读时要求报文装得下键盘位宽，
    描述符不可读时要求 6~8 字节（boot 键盘报文的合法长度）；并且**描述符里必须
    真的声明了键盘集合**才允许按谎报的 `proto=Keyboard` 兜底。
  - 鼠标认领不再允许"认领了却解析不出字段"：`proto=Mouse` 但描述符没读懂时回落到
    固定 8 字节格式（`CAP_MOUSE_BOOT`），而不是把报文静默吞掉。
- 用例：`test_report_len_table()`、`test_mouse_short_report_rejected()`（现场 `BE EF`）、
  `test_idless_kb_mouse_composite()`（无 ID 键鼠同报文：两份解析器并行、短报文交宿主）、
  `test_boot_fallback_length_guard()`（boot 兜底 + 谎报 proto 的 Consumer 接口不认领）、
  `test_win60_composite_regression()`（真机 WIN60 itf1 的 Mouse ID=2 / Keyboard ID=5）。
  修复前现场报文会产生 `dx=-4162` 的 mouse 事件、2 字节报文会解出 0xE0/0xE5/0xFB 按键。

### 12. boot 键盘把键码槽里的 0 当"结束标记"，空槽之后的键全丢

固定格式（boot 协议 / `CAP_KB_BOOT`）键盘的键码数组里，**0 表示"这一槽没有键"，
不是"后面都没有键"**。旧实现两个循环都写成"遇到 0 就停"：

```c
for (uint8_t i = 0; i < key_count && keys[i]; i++)                                  /* 按下检测 */
for (uint8_t i = 0; i < HIDKIT_MAX_KEYS_PER_KB && st->last_keyboard_keys[i]; i++)    /* 释放检测 */
```

部分键盘按**固定槽位**上报（松开的键留空、不左移），报文形如
`{0x04, 0x00, 0x05}`（a 与 b 之间空一槽）：只输出 a，b 永远不出现；松开时同理，
b 的松开也不会补发 —— 映射引擎里该键会卡住。

- 修法：`src/hid_dispatch.c` 两处改为跳过空槽（`if (keys[i] == 0) continue;`），
  释放检测按 `last_keyboard_keys[]` 全量扫描；`hid_dispatch_reset()` 的卸载补发
  同样改掉（否则卸载也补不齐）。
- 用例：`test_boot_keyboard_zero_gap()`（`{0x04,0x00,0x05}` → 必须出 a、b 两个按下，
  全零报文 → 两个松开）。修复前只有 1 个事件。
- 为什么一直没被发现：样本用例与真机抓包里的 boot 键盘都是"键码左移紧凑排列"，
  中间不留空；只有固定槽位上报的键盘才暴露。

### 13. 配置宏调小后越界写（`HIDKIT_NKRO_BYTES` / `HIDKIT_MAX_SLOTS`）

`HIDKIT_NKRO_BYTES` 是**按 keycode 索引**的 shadow 位图（键码空间 0x00..0xFF），
旧实现直接按 keycode 索引，宏调小时越界：

```c
uint8_t *cell = &dev->last_keys[keycode >> 3];        /* nkro_key_edge */
array_now[kc >> 3] |= ...;                            /* 数组段收集 */
for (uint16_t kc = 0; kc < 256; kc++) ...             /* 数组段差集 / 卸载补发 */
```

`-DHIDKIT_NKRO_BYTES=8` 喂全键位图键盘时报
`index 9 out of bounds for type 'uint8_t[8]'`（ASan/UBSan）；release 构建下会踩坏
相邻的 `array_prev`，甚至相邻槽位的状态。

另一处：槽位号是 `int8_t`，`HIDKIT_MAX_SLOTS > 127` 时
`(int8_t)HIDKIT_MAX_SLOTS` 变成负数，所有守界判断把合法槽位全挡掉 ——
挂载返回 −2，一个设备都认不了，且没有任何编译期提示。

- 修法：
  - 新增 `HID_NKRO_KEY_MAX`（= `HIDKIT_NKRO_BYTES*8 - 1`），`nkro_key_edge()`、
    数组段收集、差集循环、`hid_nkro_release_all()` 全部以它为界；
    超出容量的键**按设计丢弃**（少键而不越界）。
  - `hidkit_config.h` 增加 `_Static_assert(HIDKIT_MAX_SLOTS <= 127, ...)`，
    超大配置在编译期就报错，而不是运行时静默失效。
- 用例：`test_nkro_bytes_config_clip()`（`-DHIDKIT_NKRO_BYTES=8` 下 0xFF 键必须
  被丢弃且不越界、缓冲内的键仍正常）；其余 NKRO 用例的断言改为**按容量计算**
  期望事件数，因此 `-DHIDKIT_NKRO_BYTES=1/2/3/8/31` 全绿。
- 顺带修掉：`HIDKIT_ENABLE_GAMEPAD=0` 时 `k_xinput_btn_map` 未被引用，
  `-Wall -Wextra -Werror` 下库本身编不过（等于该裁剪开关不可用）。

相关用例：`test_evict()` 按 `HIDKIT_OVERFLOW` 分支断言（DROP_NEW 下应是
`HIDKIT_ERR_NO_SLOT` 而不是挤出），手柄用例在 `HIDKIT_ENABLE_GAMEPAD=0` 下自动跳过。

---

### 14. 代码审计：描述符解析与槽位管理的九处缺陷

以下九条来自一次**独立审计**（对着真实源码逐条写探针复现，全部在修复前的实现上
观测到事件级后果），样本用例在 `tests/host/test_hidkit.c` 的「代码审计回归样本」
一节（`test_audit_*`）。

| # | 位置 | 触发 | 后果 |
|---|---|---|---|
| a | `hid_parser.c` 两处 `0x81` 非键盘页分支 | 媒体键 Input 之后紧接 `19 E0 29 E7` 修饰键范围 | 局部 usage 未被消费，残留的 `Usage(0xE9)` 压过 range → 8 位修饰键位图变成 8 个假的 0xE9；按 Shift 出音量键 |
| b | 同上，`default` 分支 | Output/Feature 携带 Usage 后接无 usage 的 Input | 该 Input 继承上一个 usage，鼠标里多出偏移错位的 X 轴字段 |
| c | `hid_parser.c` 两处 Usage 解析 | `0x0A/0x0B`（2/4 字节 Usage，规范合法写法） | 落进 default → usage 全丢；鼠标被认领却 `idx_x/y = 0xFF`，报文恒被"消费"却零事件 |
| d | `hid_parser.c` 四处长 item 跳过 | 描述符含任意 `0xFE` 长 item | 漏跳 `bLongItemTag`，标签字节被当成下一个 item 前缀 → 其后整份描述符错位 |
| e | `hid_parser.c` `mouse_field_body` 首帧 | 首帧报文里已有按键按下 | `initialized` 抑制首帧边沿 → 按下永久丢失，松开时补一个孤儿 UP |
| f | `hid_parser.c` `lens_find` 未命中 | 描述符 Report ID 多于长度表容量（8） | 第 9 个 ID 的任意短杂报文重新被接受 —— 正好把第 11 条修掉的问题打开 |
| g | `hid_parser.c` range 余数 | `Report Count 300` + 2 项 usage range | `(uint8_t)` 截断余数（298→42），后续字段偏移整体错位 |
| h | `hidkit_dev.c` boot 解码器调用 | `CAP_*_BOOT` 路径收到 ≥256 字节报文 | `(uint8_t)len` 截断（264→8），被当成合法 boot 报文解出假位移/假按键 |
| i | `hidkit_dev.c` `hidkit_init` / `slot_release` | 按着键 `hidkit_init()` 后重挂同一设备；手柄卸载 | `s_edge[]` 未清 → 同一按键零事件；`gamepad_hid_umount()` 从未被调用 → 卸载后 `gamepad_is_active()` 仍为真、槽位复用后旧手柄 VID:PID 仍匹配 |

另外两处**不是解析缺陷、但会让宿主卡键**：

- **XInput 路径从不填 `btn_map`**（`hidkit_xinput_report`）：`slot_release()`
  的松开循环以 `map != NULL` 为前提，于是按住 A+Start 卸载后 0 个松开事件，
  上层映射引擎里这些键永久按下。修法：入口处记下布局表。
- **`HIDKIT_ENABLE_GAMEPAD=0` 时仍认领 `proto=GAMEPAD`**：槽位被永久占住，
  但没有任何解析器接手（`hidkit_report` 恒返回 false）。修法：该配置下直接
  返回 `HIDKIT_UNHANDLED`。

**审计中判定不成立的一条**：`read_signed` 的"3 字节符号扩展缺失"。
HID 的 item 只有 0/1/2/4 字节四种数据宽度（`bSize=3` → 4 字节），不存在
3 字节形式；实测 `17 FF FF FF FF`（4 字节 Logical Minimum = −1）本来就
正确解析为 -1。未改动该函数。

---

## 真机验证（USB gadget 回环）

`tests/linux/` 下有一套用 Linux USB Gadget 在真实枚举下跑 hidkit 的工具
（`hidkit_hidraw.c` + `verify_gadget.py`）：造出真机描述符的 HID 设备，
gadget 口回接到主机，再用 hidraw 取描述符/报文喂给 hidkit。
目前 4 个用例（Compx 复合接口、AJAZZ 复合接口、256 位 NKRO、同 Report ID 键鼠）
全部通过。上面第 10 条就是它抓到的。详见 `tests/linux/README.md`。

---

## 解析增强（对照成熟实现）

以下几条不是真机缺陷，而是对照成熟实现（Linux `hid-input` 的逐字段模型、
BTstack HID Parser 的单遍 usage 迭代器、hidrd 的 item 覆盖）补齐/改进的解析行为。
每条都有回归用例，且在增强前的实现上会红。

### A. 键鼠共用同一 Report ID 时并行解析

一个接口可能把键位段与鼠标字段放在**同一个 Report ID** 里（带宏的键盘常见）。
旧路由是"键盘优先"，同一份报文只会交给 `hid_nkro_dispatch()`，鼠标那半边被吃掉。
现在当两种能力都由**显式 Report ID** 命中时，两份解析器各按自己的位偏移并行处理
（对齐 Linux 逐字段模型）；没有 Report ID 的歧义报文由第 11 条的长度校验与
匹配强度择优处理。
判据：`hid_nkro_uses_report_id()` / `hid_mouse_uses_report_id()`。
用例：`test_combo_same_report_id`。

### B. 鼠标轴改为逐字段扫描（支持多个鼠标集合）

`hid_mouse_dispatch()` 原来靠 `idx_x/idx_y/idx_wheel`（每个轴只记**第一个**匹配
字段，见第 5 条旧限制）。现在按键、位移都按**字段自身的 usage + Report ID** 取数，
同一接口里两个鼠标集合（各自 Report ID）都能解析，轴值按字段累加。
用例：`test_multi_mouse_report_ids`。

### C. `Report Count` 用 16 位（支持 256 位全键位图）

`Report Count`（`0x95`/`0x96`）原来读进 `uint8_t`，2 字节形式的 **256** 会被截断成 0
→ NKRO 位图段整个丢失。现在计数与 `hid_nkro_span_t.count` 都是 16 位，
`0x96 00 01`（256）能正确展开成 usage `0x04..0xFF`。
用例：`test_nkro_report_count_256`（bit255 → usage 0xFF）。

### D. `Push(0xA4)` / `Pop(0xB4)` 全局状态栈

这两个 Main item 原来只被跳过，于是 "Push → 改 Usage Page → Pop" 的描述符在 Pop
之后仍沿用改过的页，后续字段归属错乱。两条解析路径（通用字段解析 + NKRO）都补上了
全局状态栈（Usage Page / Report Size / Report Count / Report ID / Logical Min/Max，
深度 4）。行为对齐 hidrd / Linux。
用例：`test_parser_push_pop`。

---

## 另外几处不是缺陷、但值得知道

- **固定 8 字节鼠标路径**（`hid_dispatch_mouse`）只接受 `len == 8`；这是设备侧固定
  格式的约定，宿主侧鼠标请走描述符路径（`hid_mouse_parse` + `hid_mouse_dispatch`）。
- **鼠标无描述符时静默丢弃**：`proto == MOUSE` 但 `report_desc == NULL`（描述符超过宿主
  枚举缓冲）时，本库按固定 8 字节格式（`CAP_MOUSE_BOOT`）接管；若设备实际不是 8 字节
  格式，报文长度不符时 `hid_dispatch_mouse()` 会丢弃它。宿主可用
  `hidkit_mount()` 的返回值与 `hidkit_is_active()` 自行决定要不要走兜底格式。
- **描述符容量**：通用解析器最多 `HID_PARSE_MAX_FIELDS`(64) 个字段、鼠标
  `HID_MOUSE_MAX_FIELDS`(24) 个字段；字段数触顶后不再收录，但位偏移仍按
  Report Count/Size 继续推进（增强后不再"停止累加"），后续字段偏移不会错。
