#ifndef HIDKIT_INTERNAL_H
#define HIDKIT_INTERNAL_H

/*
 * 库内部接口：解析层通过这几个函数把事件送到出口（hidkit_dev.c 实现）。
 *
 * 分层的意义：解析层（hid_parser / hid_dispatch / gamepad）只依赖这几个声明，
 * 既不知道调用方是谁，也不碰 TinyUSB/平台 API —— 因此可以在主机上直接跑样本测试。
 * 出口层负责：调用用户的回调、套用可选拦截钩子（hidkit_hooks.h）、记录槽位状态。
 */

#include <stdint.h>
#include <stdbool.h>

#include "hidkit_config.h"

/* 按键事件（键盘/鼠标按键/手柄按键，code 已含段前缀） */
void hidkit_emit_key(int8_t slot, uint16_t code, bool pressed);

/* 鼠标位移与滚轮（仅非零时调用） */
void hidkit_emit_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel);

/* 手柄绝对状态（每份解析成功的报文都调用，不做去重） */
void hidkit_emit_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                             int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt);

/* 槽位是否仍在使用（解析层用它做早期返回，避免回调打到已卸载的槽位） */
bool hidkit_slot_alive(int8_t slot);

#endif /* HIDKIT_INTERNAL_H */
