/*
 * 出口函数的弱默认实现（空实现）。
 *
 * 用法：在自己的工程里定义同名函数即可覆盖。典型写法是按 code 的段前缀分流：
 *
 *   void hidkit_input_key(int8_t slot, uint16_t code, bool pressed) {
 *       switch (HIDKIT_CODE_SEG(code)) {
 *       case HIDKIT_CODE_KEYBOARD:  // code & 0xFF 是 HID Usage ID
 *       case HIDKIT_CODE_MOUSE:     // code & 0xFF 是鼠标按键序号
 *       case HIDKIT_CODE_GAMEPAD:   // code & 0xFF 是 BTN_*
 *       default: break;
 *       }
 *   }
 *
 * 不定义也不会链接失败（库内是弱符号）。
 */

#include "hidkit.h"

#if defined(__GNUC__)
#define HIDKIT_WEAK __attribute__((weak))
#else
#define HIDKIT_WEAK
#endif

HIDKIT_WEAK void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
    (void)slot;
    (void)code;
    (void)pressed;
}

HIDKIT_WEAK void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    (void)slot;
    (void)dx;
    (void)dy;
    (void)wheel;
}

HIDKIT_WEAK void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                                          int32_t rs_x, int32_t rs_y,
                                          int32_t lt, int32_t rt)
{
    (void)slot;
    (void)ls_x;
    (void)ls_y;
    (void)rs_x;
    (void)rs_y;
    (void)lt;
    (void)rt;
}

HIDKIT_WEAK void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid)
{
    (void)slot;
    (void)vid;
    (void)pid;
}

/* 库内诊断出口的弱默认实现：什么都不做。
 * 想看到 HIDKIT_DEBUG=1 时的库内自证信息，就在自己工程里实现这个函数并接到日志。 */
HIDKIT_WEAK void hidkit_debug_printf(const char *fmt, ...)
{
    (void)fmt;
}
