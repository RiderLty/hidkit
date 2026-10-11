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

XInput（Xbox 手柄）的**协议知识**在这里（固定布局 + 归一化入口 `hidkit_xinput_report()`），
但它的**端点管线**（TinyUSB 类驱动、Xbox 初始化握手）在独立仓库
[**hidkit-tusb-xinput**](https://github.com/RiderLty/hidkit-tusb-xinput) 里。

注意：**适配器不由本仓库挂载**。core 零平台依赖、不引用任何 USB 栈头文件，
所以本仓库里没有、也不会有 submodule —— 谁用谁挂（`git submodule add ... lib/hidkit-tusb-xinput`），
接入步骤见该仓库 README 的「接入清单」，适配器与 core 的分工约定见
[`adapters/README.md`](adapters/README.md)。换个 USB 栈（CherryUSB / ESP-IDF …）
只要另写一个适配器，core 一行都不用改。

Switch Pro 也采用相同的分层方式：HID 报文解析在本库，USB 握手和传感器初始化在
[**hidkit-tusb-switch-pro**](https://github.com/RiderLty/hidkit-tusb-switch-pro) 中。
本库不会主动发送 USB 报文，也不会注册 TinyUSB 类驱动；如果项目需要支持 Pro，
把该适配器加入已有的 HID 主机即可。适配器识别 `057e:2009`（Pro）以及
`057e:2006/2007`（Joy-Con USB 接口），完成握手后再把同一份输入报文交给
`hidkit_report()`。

### Switch Pro 握手流程

Pro 手柄枚举后不会立即发送完整的 `0x30` 输入流，宿主必须按顺序执行下面的
非阻塞握手。每个设备地址都要维护自己的包序号（`0..15` 循环）和 OUT 忙状态：

1. 发送 `80 02`，等待 `0x81 02` 应答；发送 `80 03` 切换手柄内部 UART 到 3 Mbps；
   再发送一次 `80 02`；然后发送 `80 04` 关闭 USB 超时。
2. 发送输出报文 `01 seq [8-byte neutral rumble] subcommand data`：
   `48 01` 开启震动、`03 30` 切换标准全量报告、`40 01` 开启 IMU。
   子命令在报文偏移 `10`，参数从偏移 `11` 开始；OUT 入队成功不等于命令成功，
   必须等待对应的 `0x21` ACK，超时后有限重试。
3. 通过子命令 `10` 读取 SPI 校准区：摇杆工厂/用户校准、`0x6020` IMU 校准、
   `0x6080` 平放参考和用户校准魔数。回包要核对 ACK、回显地址、长度和数据完整性；
   用户校准不可用时回退工厂值或缺省值。

握手状态机只在宿主的周期任务中推进，不在 HID 回调里阻塞等待。宿主应把每份输入
报文先交给 `switch_pro_report()`（它只消费握手 ACK/SPI 回包），再交给
`hidkit_report()`；`0x21/0x81` 控制应答不能当作手柄输入状态，否则会产生未初始化
的轴、按键事件。卸载时调用 `switch_pro_umount()`，重新枚举会从头开始握手。

握手库交付的校准结构使用传感器原生轴；hidkit 的布局负责摇杆归一化和六轴单位转换：
陀螺仪输出约定为 `±32767 = ±2000 dps`，加速度为 `±32767 = ±4 g`，静止时加速度
保留重力分量。Pro 在校准后映射为 DS5 兼容轴序 `(X,Y,Z) → (-Y,Z,-X)`。
完整的 TinyUSB 接入清单、平台端口函数和其他 USB 栈用法见该独立库 README。

---

## 内存模型

- **没有堆分配，没有上下文对象**：库内全是 `static` 数组，槽位数量由 `src/hidkit_config.h`
  的宏一次定死（`HIDKIT_MAX_KEYBOARDS` / `HIDKIT_MAX_MICE` / `HIDKIT_MAX_GAMEPADS`）。
- 槽位耗尽时的策略由 `HIDKIT_OVERFLOW` 决定：`EVICT_IDLE`（默认，挤掉最久没有报文的，
  并先补发"全部抬起"）或 `DROP_NEW`（不解析，回调 `dropped` 告知）。
- 需要"最久没有报文"这个判据时定义 `HIDKIT_TICK_MS()`（毫秒单调递增）；不定义就退化为
  "挤掉最先分配的槽位"，不需要任何时间函数。
- Pico 上想把热路径放进 SRAM：定义 `HIDKIT_HOT` 为 `__not_in_flash_func(...)` 之类；默认展开为空。
  标注范围是**每报文都要跑的入口 + dispatch/parse 层**（挂载期解析描述符、查询类辅助函数不标）。
  展开式必须连标识符一起吐出来，与 SDK 的宏同构：

  ```c
  #define HIDKIT_HOT(f) __not_in_flash_func(f)                              // Pico SDK
  #define HIDKIT_HOT(f) __attribute__((section(".time_critical.hidkit"))) f  // 裸属性
  ```

  注意这只能停摆**本库这半边**：热路径的最后一步是宿主实现的那几个 `hidkit_input_*`
  弱符号，它们也得由宿主放进 RAM，整条链才真正不在 flash 写窗口里卡住。

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
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt,
                              uint32_t buttons);

/* 手柄六轴运动数据（陀螺仪 + 加速度计）。量程约定见下节 */
void hidkit_input_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                 int16_t ax, int16_t ay, int16_t az);

/* 触摸板快照（通用格式，DS5/DS Edge 起步）。每份解析成功的报文都回调，
 * 去重是消费方策略；umount 补发一次全零快照（"设备已离开"信号）。
 * meta 位域：bit0-9 触点在位 / bit10-13 最大点数 / bit14-19 来源枚举；
 * dims：bit0-15 宽 / bit16-31 高；坐标 = 设备原生分辨率，无效槽位恒 0 */
void hidkit_input_touchpad(int8_t slot, const hidkit_touchpad_t *tp);

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

### 六轴量程约定（新布局必须照此缩放）

手柄六轴（陀螺仪 + 加速度计）与摇杆共用同一套"满量程 = ±32767"的归一化约定，
**基准取 DS5（DualSense / DualSense Edge）满量程**：

| 量 | 满量程 | 1 unit | 换算公式（非 DS5 布局用） |
|---|---|---|---|
| 陀螺仪 | **±32767 = ±2000 dps** | ≈ 0.0610 dps | `out = dps × 32767 / 2000` |
| 加速度 | **±32767 = ±4 g** | ≈ 0.122 mg | `out = mg × 32767 / 4000` |

规范条款（新增手柄布局时必须遵守）：

1. **DS5/DS Edge 原始计数即该比例**（±32768 ↔ 满量程），`ds5_parse` 直通即可；
   其他布局按上表公式缩放到 `-32767..32767`。
2. **钳位**：raw = -32768 时钳到 -32767（值域闭区间，与摇杆约定一致）。
3. **DS5、DS Edge 与 Switch Pro 使用相同坐标系**（X=俯仰 pitch、
   Y=偏航 yaw、Z=横滚 roll）。Pro 校准后按 `(-Y, Z, -X)` 转为 PlayStation
   原生轴序；Joy-Con 保留原生坐标系，消费方按持握方式映射。
4. **加速度含重力**：静止时重力向量模长约 8192（1 g）。零偏与重力
   补偿是消费方策略，库内不做。
5. **`has_motion` 必须如实声明**：布局报文含有效六轴时置 1，否则恒 0。
   无 IMU 的手柄**不发**运动事件 —— 0 对陀螺仪是合法读数（"完全静止"），
   发 0 是伪造读数。
6. **umount 补发一次全零**：消费方没有 umount 通知，补零就是"设备已离开"
   的信号（角速度停在旧值上会让积分式解算永久漂移）。

锚点宏 `HIDKIT_MOTION_GYRO_FULL_SCALE_DPS`（2000）与
`HIDKIT_MOTION_ACCEL_FULL_SCALE_MG`（4000）在 `include/hidkit.h`。

### 可选拦截钩子（同样是弱符号）

`hidkit_hook_key()` / `hidkit_hook_mouse_abs()` / `hidkit_hook_gamepad_abs()` /
`hidkit_hook_gamepad_motion()`：
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

CherryUSB / ESP-IDF / nRF 等都照这四步；**带完整代码的接线说明**（含每个字段怎么填、
设备类型怎么判定）见 [`examples/README.md`](examples/README.md)。

XInput 这类**没有 HID 接口**的设备多一步：由
[hidkit-tusb-xinput](https://github.com/RiderLty/hidkit-tusb-xinput) 适配器把厂商报文解析成
`hidkit_xinput_pad_t` 后调用 `hidkit_xinput_report()`，需要一个类驱动注册钩子
（`usbh_app_driver_get_cb()`，全工程只能有一个定义）。**接入清单、自检日志与常见错误**
见该仓库 README；[`examples/README.md`](examples/README.md) 也有一份精简版。

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

真机验证（可选，需要 Linux + USB gadget 回环，如树莓派 5）：`tests/linux/` 用
ConfigFS 造出真机描述符的 HID 设备，在**真实 USB 枚举**下跑 hidkit 并断言事件，
见 [`tests/linux/README.md`](tests/linux/README.md)。

已知问题与残余限制见 [`KNOWN_ISSUES.md`](KNOWN_ISSUES.md)：第一版是**忠实移植**，
移植时故意保留的 5 条描述符解析缺陷现已修复，每条都补了样本用例把规范行为固定住
（`tests/host/test_hidkit.c` 末节「缺陷修复样本」，在修复前的实现上会红）；
移植完成后又修了 `Report Count = 0`、一处移植回归（手柄布局表恒不命中）、
真机复合设备的分类/数组松开，以及一套对照成熟实现（Linux `hid-input`、hidrd 等）
的解析增强，末尾还列了仍未实现的部分（描述符容量上限等）。

---

## 来源与许可

代码取自一套**经过实际设备验证**的 HID/XInput 解析实现
（键鼠描述符解析、NKRO、boot 协议、DS5/DS Edge/Azeron/XInput 手柄布局）。
移植时只做了三类改动：去掉平台依赖（Pico SDK 宏与应用侧业务逻辑）、
统一事件出口与 code 空间、槽位管理归本库。

以 [MIT 许可证](LICENSE) 发布（Copyright (c) 2026 RiderLty）。
