# examples

本目录暂时不放"可直接编译"的示例工程（那需要绑定具体 USB 栈的构建系统）。
**两份权威参考**：

| 参考 | 什么时候看它 |
|---|---|
| [`tests/host/test_hidkit.c`](../tests/host/test_hidkit.c) | 想看"描述符 + 报文 → 事件序列"的行为规格；不需要硬件，`ctest` 直接跑 |
| [pico-hid-debugger](https://github.com/RiderLty/pico-hid-debugger) | 想看**真实接线**：Pico 2 + TinyUSB 主机栈，HID 键鼠/手柄与 XInput 适配器共存的完整工程。`src/hidkit_app.c` 就是接线层，照抄即可 |

---

## HID 设备接线（任何 USB 栈都是这四步）

```c
hidkit_init();                                  /* 1. 上电一次（在栈初始化之前） */
int8_t slot = hidkit_mount(&info);              /* 2. 挂载：<0 = 本库不认，交回你自己处理 */
bool used = hidkit_report(slot, buf, len);      /* 3. 收到报文 */
hidkit_umount(slot);                            /* 4. 卸载：库内先补发"全部抬起" */
```

出口函数在**自己的工程里定义同名函数即覆盖**（库内是弱符号空实现，不定义也能链接，
只是收不到事件）：

```c
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed);          /* 键/鼠/手柄按键统一出口 */
void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel);
void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt);
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid);       /* 可选诊断 */
```

`info` 是 `hidkit_dev_info_t`：宿主只需填七个成员
（`vid / pid / dev_addr / itf / proto / report_desc / report_desc_len`）。
以 TinyUSB 为例，在 `tuh_hid_mount_cb` 里这样填：

```c
hidkit_dev_info_t info = {
    .vid = vid, .pid = pid,
    .dev_addr = dev_addr,
    .itf = itf_num,
    /* TinyUSB 的 hid_interface_protocol_enum_t 与 HIDKIT_PROTO_* 数值相同，但仍显式映射：
     * 本库不依赖任何栈的头文件，这层对应关系归宿主 */
    .proto = (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) ? HIDKIT_PROTO_KEYBOARD
           : (itf_protocol == HID_ITF_PROTOCOL_MOUSE)    ? HIDKIT_PROTO_MOUSE
                                                         : HIDKIT_PROTO_NONE,
    .report_desc = desc_report,      /* 可为 NULL：固定布局设备（boot 键鼠）不需要描述符 */
    .report_desc_len = desc_len,
};
```

设备类型怎么判定（决定了"要不要描述符"）：`proto = KEYBOARD` 走 boot 固定格式、
**不看描述符**；`proto = MOUSE` 走描述符解析（描述符没抓到就静默丢弃）；
`proto = NONE` 先按 VID:PID 匹配已知手柄、再尝试键盘描述符（位图与 6KRO 数组都认），
都不中则返回 `HIDKIT_UNHANDLED`（这是扩展点，宿主可自行接管）。
详见 [`KNOWN_ISSUES.md`](../KNOWN_ISSUES.md) 与库内注释。

---

## XInput（Xbox 手柄）：多一个适配器 + 一个类驱动钩子

Xbox 360 / One 在 USB 上**不是 HID 设备**（走厂商接口与厂商报文，没有可用的报告描述符），
上面那套"描述符 → 布局"的通路对它无效。这一段由独立仓库
[**hidkit-tusb-xinput**](https://github.com/RiderLty/hidkit-tusb-xinput) 补齐：

```bash
git submodule add https://github.com/RiderLty/hidkit.git             lib/hidkit
git submodule add https://github.com/RiderLty/hidkit-tusb-xinput.git lib/hidkit-tusb-xinput
```

```cmake
add_subdirectory(lib/hidkit)
add_subdirectory(lib/hidkit-tusb-xinput)
target_link_libraries(your_app PRIVATE hidkit hidkit_tusb_xinput tinyusb_host)
```

```c
/* tusb_config.h */
#define CFG_TUH_XINPUT 1                  /* 想支持几个 XInput 手柄就填几 */
#define CFG_TUH_ENUMERATION_BUFSIZE 512   /* Xbox 360 无线接收器的配置描述符 > 321 字节 */

/* 消费方唯一的类驱动注册点：全工程只能有一个定义，把适配器的驱动转发出去 */
#include "hidkit_xinput_glue.h"

usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return hidkit_tusb_xinput_driver();    /* = &usbh_xinput_driver */
}
```

三个 `tuh_xinput_*` 回调（mount / report / umount）由适配器的 glue 实现，
**不要**在自己的工程里再定义一遍（会抢符号，而且会破坏它的"报文重订阅"）。

接上之后，XInput 手柄的按键与轴会和 HID 手柄走**同一批出口函数**
（`hidkit_input_key` 的 `0x0200` 段 + `hidkit_input_gamepad_abs`），
消费方不需要为 XInput 写第二套业务逻辑。

> **完整接入清单**（容量对齐、自检日志、常见错误逐条）见适配器仓库 README 的
> [「接入清单」](https://github.com/RiderLty/hidkit-tusb-xinput#接入清单从零到跑通照做即可)一节。
> 适配器与 core 的分工约定见 [`../adapters/README.md`](../adapters/README.md)。

---

## 计划补充

| 目录 | 内容 |
|---|---|
| `pico_tinyusb/` | 从 pico-hid-debugger 抽出的最小接线示例（只留 hidkit 相关那几十行） |
| `host_demo/` | 与 `tests/host` 同源的可执行 demo，打印解析出来的事件流（便于快速肉眼验证新设备） |
