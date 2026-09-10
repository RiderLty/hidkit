# adapters —— 各 USB 栈 / 各设备类别的适配层

core（`src/`）只做解析，不认识任何 USB 栈。凡是"拿不到 HID 接口、必须自己跑端点管线"
的设备类别，都放在这里，各自独立成仓，用 git submodule 挂进来。

约定：

- 适配器**只做三件事**：① 从栈里拿到设备基本信息（vid/pid/addr/itf/report_desc）；
  ② 跑该类别特有的端点/握手流程；③ 把结果交给 core 的标准入口。
- 适配器**不要**在 core 里加分支；core 侧只保留"归一化入口"（如
  `hidkit_xinput_report()`）与"是否被消费"的返回值。
- 适配器**不要**占用栈的全局钩子（例如 TinyUSB 的 `usbh_app_driver_get_cb()`）——
  由消费方唯一的那个钩子转发，避免和别的类驱动抢符号。

## 计划中的适配器

| 目录 | 内容 | 状态 |
|---|---|---|
| `tusb_xinput/` | TinyUSB 类驱动 + Xbox 各代报文解析 + 初始化握手 → `hidkit_xinput_pad_t` | 待建（独立仓库，submodule 挂载） |
| （将来）`cherryusb_xinput/` | 同上，换成 CherryUSB 的端点 API | — |

XInput 的**布局与归一化**已经在 core 里（`src/hidkit_dev.c` 的 `hidkit_xinput_report()`），
所以适配器只需要把厂商报文解析成 `hidkit_xinput_pad_t` 并调用它即可。
