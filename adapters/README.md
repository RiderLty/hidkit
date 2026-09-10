# adapters —— 各 USB 栈 / 各设备类别的适配层

core（`src/`）只做解析，**不认识任何 USB 栈**（它的翻译单元只 include 几个 C 标准头
—— `stdint.h` / `stdbool.h` / `string.h` / `stdarg.h`——没有任何平台头或 USB 栈头文件）。
凡是"拿不到 HID 接口、必须自己跑端点管线"的设备类别（XInput 是典型），
都需要一个适配器把厂商报文喂成 core 能吃的形状。

## 适配器在哪里

**不在本仓库里**：core 不引用任何 USB 栈，所以本仓库不含、也不会包含适配器
submodule。适配器各自独立成仓，**由使用它的人挂进自己的工程**：

```bash
git submodule add https://github.com/RiderLty/hidkit-tusb-xinput.git lib/hidkit-tusb-xinput
```

本目录（`adapters/`）只放**契约**：约定写在这里，实现放在各自的仓库里。
这样"换个 USB 栈" = 换一个适配器仓库，core 与消费方都不受影响。

## 约定

- 适配器**只做三件事**：① 从栈里拿到设备基本信息（vid/pid/addr/itf/report_desc）；
  ② 跑该类别特有的端点/握手流程；③ 把结果交给 core 的标准入口。
- 适配器**不要**在 core 里加分支；core 侧只保留"归一化入口"（如
  `hidkit_xinput_report()`）与"是否被消费"的返回值。
- 适配器**不要**占用栈的全局钩子（例如 TinyUSB 的 `usbh_app_driver_get_cb()`）——
  那是消费方唯一的类驱动注册点，多个类驱动各定义一份会撞符号；适配器只导出一个
  "取值函数"（如 `hidkit_tusb_xinput_driver()`），由消费方在那个钩子里转发。
  真正的实例见 [pico-hid-debugger](https://github.com/RiderLty/pico-hid-debugger)
  的 `src/hidkit_app.c`。
- 适配器**不要**在 core 的出口函数之外另开一条输出路径：事件一律经
  `hidkit_input_key()` / `hidkit_input_gamepad_abs()` 出去，消费方只认这一套。

## 已有 / 计划中的适配器

| 仓库 | 内容 | 状态 |
|---|---|---|
| [hidkit-tusb-xinput](https://github.com/RiderLty/hidkit-tusb-xinput) | TinyUSB XInput 类驱动 + Xbox 各代报文解析 + 初始化握手 → `hidkit_xinput_pad_t` | **已建**。接入步骤、自检清单与常见错误见该仓库 README 的「接入清单」一节 |
| （将来）`hidkit-cherryusb-xinput` | 同上，换成 CherryUSB 的端点 API | — |

XInput 的**布局与归一化**已经在 core 里（`src/hidkit_dev.c` 的 `hidkit_xinput_report()`：
按钮位 → `BTN_*` 查表、扳机阈值合成数字位、Y 轴取反、模拟值缩放），所以适配器只需要把
厂商报文解析成 `hidkit_xinput_pad_t` 并调用它即可 —— 这也意味着**换 USB 栈不需要重写
任何归一化逻辑**。

## 写一个新适配器要做什么

1. 新建独立仓库，实现该栈的类驱动 / 端点管线；
2. 在挂载回调里组装 `hidkit_dev_info_t` 调 `hidkit_mount()`：
   非 HID 设备用 `proto = HIDKIT_PROTO_GAMEPAD`（让 core 直接认领手柄槽位、
   不做描述符判定），固定布局设备 `report_desc = NULL`；
3. 报文→`hidkit_xinput_pad_t`（或你自己的归一化结构 + core 新增的对应入口）→
   调 `hidkit_xinput_report(slot, &pad)`；
4. 拔出时 `hidkit_umount(slot)`（core 会先补发"全部抬起"）；
5. 导出一个"取类驱动"的函数，**不要**定义 `usbh_app_driver_get_cb()`。
