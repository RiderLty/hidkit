#ifndef HIDKIT_HOOKS_H
#define HIDKIT_HOOKS_H

/*
 * 可选拦截钩子（弱符号）。
 *
 * 用途：在事件真正送到用户回调之前做重映射 / 锁定 / 屏蔽 —— 例如把某个键
 * 改写成另一个键、把鼠标移动缩放到某个范围、或在某种状态下吞掉全部输入。
 *
 * 用法：在自己的工程里定义同名函数即可覆盖默认实现（默认全部直通）。
 *   bool hidkit_hook_key(uint16_t *code, bool *pressed) {
 *       if (*code == (HIDKIT_CODE_KEYBOARD | KEY_CAPS_LOCK)) return false; // 吞掉
 *       if (*code == (HIDKIT_CODE_KEYBOARD | KEY_A)) *code = HIDKIT_CODE_KEYBOARD | KEY_B;
 *       return true;
 *   }
 *
 * 返回值：true = 继续派发（可按需改写 *code / *pressed）；false = 丢弃该事件。
 */

#include <stdint.h>
#include <stdbool.h>

bool hidkit_hook_key(uint16_t *code, bool *pressed);
bool hidkit_hook_mouse_abs(int32_t *dx, int32_t *dy, int32_t *wheel);
bool hidkit_hook_gamepad_abs(int32_t *ls_x, int32_t *ls_y, int32_t *rs_x,
                             int32_t *rs_y, int32_t *lt, int32_t *rt);

/* 手柄六轴（陀螺仪 + 加速度计，量程约定见 include/hidkit.h 的
 * hidkit_input_gamepad_motion）。umount 补发的全零帧也经过这里，
 * 钩子可在断连帧做自己的清理；想屏蔽断连帧在此吞掉即可。 */
bool hidkit_hook_gamepad_motion(int16_t *gx, int16_t *gy, int16_t *gz,
                                int16_t *ax, int16_t *ay, int16_t *az);

#endif /* HIDKIT_HOOKS_H */
