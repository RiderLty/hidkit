#include "hidkit_hooks.h"

/*
 * 默认实现：全部直通。用 __attribute__((weak)) 声明，用户工程里定义同名
 * 非弱函数即可自动覆盖（GCC/Clang 通用；IAR 等工具链可用 --weak 等效选项，
 * 或直接改这里的实现）。
 */

#if defined(__GNUC__)
#define HIDKIT_WEAK __attribute__((weak))
#else
#define HIDKIT_WEAK
#endif

HIDKIT_WEAK bool hidkit_hook_key(uint16_t *code, bool *pressed)
{
    (void)code;
    (void)pressed;
    return true;
}

HIDKIT_WEAK bool hidkit_hook_mouse_abs(int32_t *dx, int32_t *dy, int32_t *wheel)
{
    (void)dx;
    (void)dy;
    (void)wheel;
    return true;
}

HIDKIT_WEAK bool hidkit_hook_gamepad_abs(int32_t *ls_x, int32_t *ls_y,
                                         int32_t *rs_x, int32_t *rs_y,
                                         int32_t *lt, int32_t *rt)
{
    (void)ls_x;
    (void)ls_y;
    (void)rs_x;
    (void)rs_y;
    (void)lt;
    (void)rt;
    return true;
}
