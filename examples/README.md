# examples

接线示例（按 USB 栈分目录）。**目前最完整的用法参考是
[`tests/host/test_hidkit.c`](../tests/host/test_hidkit.c)** —— 它就是一份"喂描述符与报文、
断言事件序列"的最小可用代码，且不需要硬件。

计划补充：

| 目录 | 内容 |
|---|---|
| `pico_tinyusb/` | Pico + TinyUSB host：HID 键鼠/手柄接线（`tuh_hid_*` 四步），以及 XInput 适配器的 `usbh_app_driver_get_cb()` 转发写法 |
| `host_demo/` | 与 `tests/host` 同源的可执行 demo，打印解析出来的事件流（便于快速肉眼验证新设备） |

四步接线（任何栈都成立）：

```c
hidkit_init(&callbacks);                                        /* 1. 注册回调 */
int8_t slot = hidkit_mount(&info);                              /* 2. 挂载：<0 交回你自己处理 */
bool used = hidkit_report(slot, buf, len);                      /* 3. 报文 */
hidkit_umount(slot);                                            /* 4. 卸载 */
```
