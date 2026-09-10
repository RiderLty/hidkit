#ifndef HID_DISPATCH_H
#define HID_DISPATCH_H

#include <stdint.h>

/*
 * boot 协议（固定格式）键鼠报文的边沿解析。
 *   - mouse_report_x8 / keyboard_report_x8 结构定义在 hid_struc.h
 *   - 按键边沿检测所需的 last_* 状态按 slot 分槽保存在本模块内（static）
 *
 * 为什么要按 slot 分槽：host 侧允许通过 hub 接多个键盘/鼠标。若共用一份 last_*
 * 状态，A 键盘的"按下 W"之后紧跟 B 键盘的空报文，边沿检测会立刻误判 W 松开 ——
 * 多设备下按键持续抖动。slot 由 hidkit 自己分配（见 hidkit_dev.c），与具体
 * USB 栈无关。
 */

// 解析固定 8 字节鼠标报文：按键边沿 → key 事件，位移/滚轮 → mouse_abs
void hid_dispatch_mouse(int8_t slot, const uint8_t *report, uint8_t len);

// 解析 boot 协议键盘报文（修饰键字节 + 键码槽位）→ key 事件
void hid_dispatch_keyboard(int8_t slot, const uint8_t *report, uint8_t len);

// 卸载/槽位挤出时调用：把该槽位仍按着的键/鼠标键补发一次"松开"，再清空状态。
// 不补发的话上层映射引擎会认为键还按着，由它触发的动作永远等不到松开事件。
void hid_dispatch_reset(int8_t slot);

#endif // HID_DISPATCH_H
