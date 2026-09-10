# hidkit

可移植的 HID / XInput 设备解析库：**纯 C、零平台依赖、静态内存、回调输出**。

把 USB 栈（TinyUSB / CherryUSB / ESP-IDF / 裸机自写）送来的**描述符**与**报文**
解析成标准事件，交给你的回调；键鼠与手柄走统一出口，手柄归一化为一个与厂商无关的结构。

**设计取向**：核心只做解析，不碰任何 USB 栈与 OS；因此解析逻辑既能在 Pico/ESP32/STM32
上跑，也能在 PC 上跑样本测试（本仓库的测试就是这么做的，不需要硬件）。

---

## 职责边界

| 本库负责 | 本库**不**负责 |
|---|---|
| 报告描述符解析（鼠标、NKRO、boot 键盘） | USB 枚举、端点管理、控制传输 |
| 报文 → 标准事件（按键/鼠标/手柄） | 设备驱动注册（`usbh_app_driver_get_cb` 之类的钩子） |
| 按键边沿检测、卸载补发"全部抬起" | 键位映射、宏、连发等业务逻辑 |
| 设备类型判定、槽位与容量管理 | 参数持久化、UI、日志 |

XInput（Xbox 手柄）的**协议知识**在这里（固定布局 + 归一化入口），
但它的**端点管线**（TinyUSB 类驱动、Xbox 初始化握手）在独立仓库里，
以 submodule 形式挂在 `adapters/` 下 —— 换个 USB 栈只要换那个适配器，
core 一行都不用改。

---

## 内存模型

- **没有堆分配，没有上下文对象**：库内全是 `static` 数组，槽位数量由 `src/hidkit_config.h`
  的宏一次定死（`HIDKIT_MAX_KEYBOARDS` / `HIDKIT_MAX_MICE` / `HIDKIT_MAX_GAMEPADS`）。
- 槽位耗尽时的策略由 `HIDKIT_OVERFLOW` 决定：`EVICT_IDLE`（默认，挤掉最久没有报文的，
  并先补发"全部抬起"）或 `DROP_NEW`（不解析，回调 `dropped` 告知）。
- 需要"最久没有报文"这个判据时定义 `HIDKIT_TICK_MS()`（毫秒单调递增）；不定义就退化为
  "挤掉最先分配的槽位"，不需要任何时间函数。
- Pico 上想把热路径放进 SRAM：定义 `HIDKIT_HOT` 为 `__not_in_flash_func(...)` 之类；默认展开为空。

---

## 公共接口

唯一公共头是 [`include/hidkit.h`](include/hidkit.h)。三个入口 + 一组回调：

```c
/* 设备信息：宿主要提供的最小契约（换了 USB 栈也只填这个） */
typedef struct {
    uint16_t vid, pid;
    uint8_t  dev_addr;          /* 宿主栈的设备标识，本库原样回传 */
    uint8_t  itf;               /* HID 接口号（固定布局设备填 0） */
    uint8_t  proto;             /* HIDKIT_PROTO_NONE / KEYBOARD / MOUSE */
    const uint8_t *report_desc; /* 可为 NULL（固定布局设备） */
    uint16_t report_desc_len;
} hidkit_dev_info_t;

hidkit_init();                                    /* 清空状态，一次 */
int8_t slot = hidkit_mount(&info);                /* >=0 已接管；<0 未消费，交宿主 */
bool consumed = hidkit_report(slot, buf, len);    /* 解析并触发出口函数 */
hidkit_umount(slot);                              /* 卸载：补发全部抬起后释放槽位 */
```

**"是否被消费"是本库的扩展点**：`hidkit_mount()` 返回 <0、或 `hidkit_report()` 返回 false
时，该设备/该报文交回宿主自行处理 —— 你可以据此接管厂商私有设备，也可以什么都不做。

### 回调与 code 空间

按键类事件**统一一个出口**，靠段前缀区分类型（详见 `src/hidkit_codes.h`）：

| 段 | 含义 |
|---|---|
| `0x0000 + HID Usage ID` | 键盘（0x04..0xFE，修饰键 0xE0..0xE7） |
| `0x0100 + 序号` | 鼠标按键 |
| `0x0200 + BTN_*` | 手柄按键（含由模拟扳机阈值合成出来的 LT/RT 两位） |

出口是**弱符号函数**：库里给空实现，你在自己工程里定义同名函数即覆盖 ——
没有回调注册表、没有函数指针、没有间接调用（也和 `hidkit_hook_*` 同一套机制）。

```c
/* 键盘 / 鼠标按键 / 手柄按键统一走这里，按 code 的段前缀分流。仅变化时调用 */
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed);

/* 鼠标位移与滚轮。仅非零时调用 */
void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel);

/* 手柄绝对状态：每份解析成功的报文都回调（手柄报文即绝对状态，不做去重） */
void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt);

/* 可选诊断：设备被丢弃时通知一次 */
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid);
```

用自己的写法（示例）：

```c
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
    switch (HIDKIT_CODE_SEG(code)) {
    case HIDKIT_CODE_KEYBOARD:  /* code & 0xFF 是 HID Usage ID */ break;
    case HIDKIT_CODE_MOUSE:     /* 鼠标按键序号 */               break;
    case HIDKIT_CODE_GAMEPAD:   /* BTN_* */                      break;
    }
}
```

slot 用来区分设备（多键盘/多鼠标各占一个）。不定义这些函数也能编译链接，只是收不到事件。

以后新增 HID 设备类型（消费类媒体键等）只需**加一段前缀**，不必改回调签名。

### 可选拦截钩子（同样是弱符号）

`hidkit_hook_key()` / `hidkit_hook_mouse_abs()` / `hidkit_hook_gamepad_abs()`：
在出口函数**之前**调用，可**改写或吞掉**事件（做重映射、锁定、屏蔽），默认直通。
见 [`src/hidkit_hooks.h`](src/hidkit_hooks.h)。

**两层弱符号的分工**：`hidkit_hook_*` 管"事件要不要发、发成什么"，`hidkit_input_*` 管"发出去之后怎么用"。
只做映射的话用 hook；要接自己的输入管线就用 input。

---

## 集成

### 作为 submodule

```bash
git submodule add <本仓库 URL> lib/hidkit
```

```cmake
add_subdirectory(lib/hidkit)
target_link_libraries(your_app PRIVATE hidkit)
```

`hidkit` 是 INTERFACE target（用你的工具链编译，零平台依赖）。
嵌入式工程建议加 `-DHIDKIT_BUILD_TESTS=OFF` 跳过主机测试。

### 改容量 / 开关

命令行或自己的配置头都行：

```cmake
target_compile_definitions(your_app PRIVATE
    HIDKIT_MAX_KEYBOARDS=1
    HIDKIT_MAX_MICE=1
    HIDKIT_MAX_GAMEPADS=1
    HIDKIT_ENABLE_ADAPTER_AZERON=0)   # 不需要 Azeron 就关掉，省代码
```

更集中的做法：定义 `HIDKIT_USER_CONFIG` 指向你的 `hidkit_user_config.h`。

### 接一个新的 USB 栈

只要满足"能提供基本信息"这一条：

1. 栈初始化的地方调用 `hidkit_init()`，并定义自己需要的 `hidkit_input_*` 出口函数；
2. 设备挂载回调里填一个 `hidkit_dev_info_t`（vid/pid/addr/itf/proto/report_desc）→ `hidkit_mount()`；
3. 收到中断报文 → `hidkit_report(slot, buf, len)`；
4. 卸载 → `hidkit_umount(slot)`。

CherryUSB / ESP-IDF / nRF 等都照这四步。XInput 这类**没有 HID 接口**的设备，
由对应适配器把厂商报文解析成 `hidkit_xinput_pad_t` 后调用 `hidkit_xinput_report()`。

### 加一个新设备（手柄）

手柄布局是**数据表**：`src/layouts/` 下每个厂商一个头文件，内容是
`{ vid, pid, parse 函数, button_map 表 }`。新增设备照抄一个最接近的即可；
若设备需要"非 HID 通道"（像 Azeron 那样轴走手柄通道、键盘走 boot 键盘通道），
在 `hidkit_dev.c` 的 `HIDKIT_ENABLE_ADAPTER_*` 分支里加规则，或返回"未消费"让宿主自己处理。

---

## 测试

主机侧样本测试，不需要硬件：

```bash
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

用例本身就是**行为规格**：喂"描述符 + 报文"，断言事件序列。开着
`-fsanitize=address,undefined,alignment`，所以未对齐访问（Cortex-M0 会 HardFault 的那类）
在主机上就能被抓到。

已知问题与残余限制见 [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md)：第一版是**忠实移植**，
移植时故意保留的 5 条描述符解析缺陷现已修复，每条都补了样本用例把规范行为固定住
（`tests/host/test_hidkit.c` 末节「缺陷修复样本」，在修复前的实现上会红）；
移植完成后又修了两条（`Report Count = 0` 的规范符合性、以及一处**移植引入的回归**：
HID 手柄布局表曾被槽位守界挡住探测调用而恒不命中），同样各带回归用例；
仍未实现的部分（Push/Pop、描述符容量上限等）也逐条列在里面。

---

## 来源与许可

代码提取自 [pico-hid-mapper](https://github.com/RiderLty/pico-hid-mapper) 中经过实际设备
验证的 HID/XInput 解析部分（键鼠描述符解析、NKRO、boot 协议、DS5/Azeron/XInput 手柄布局）。
移植时只做了三类改动：去掉平台依赖（Pico SDK 宏、应用侧的 core/makcu/授权逻辑）、
统一事件出口与 code 空间、槽位管理归本库。

以 [MIT 许可证](LICENSE) 发布（Copyright (c) 2026 RiderLty）。
