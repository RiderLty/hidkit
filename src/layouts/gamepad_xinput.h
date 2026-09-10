/**
 * @file gamepad_xinput.h
 * @brief XInput 手柄转换层：hidkit_xinput_pad_t → gamepad_state_t 归一化
 *
 * XInput 按钮位是固定的（微软定义），无需按 VID/PID 匹配 —— 直接用一份查表。
 * 所有 XInput 设备（Xbox 360/One/OG）的正常化数据结构相同，统一为
 * hidkit_xinput_pad_t（见 hidkit.h），由宿主栈的 XInput 适配器填充。
 *
 * 用法：
 *   gamepad_state_t gs;
 *   xinput_to_gamepad_state(&pad, &gs);
 *   → gs.buttons 的 bit N 含义查 gs.btn_map[N]（BTN_* / DPAD_*）
 */

#ifndef GAMEPAD_XINPUT_H
#define GAMEPAD_XINPUT_H

#include "gamepad.h"
#include "hidkit.h"     // hidkit_xinput_pad_t（XInput 归一化输入）
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * XInput 按钮 → 通用 BTN_* 查表
 *
 * hidkit_xinput_pad_t.buttons 的 bit 定义：
 *   0x0001 DPAD_UP      0x0002 DPAD_DOWN    0x0004 DPAD_LEFT
 *   0x0008 DPAD_RIGHT   0x0010 START         0x0020 BACK
 *   0x0040 LS           0x0080 RS            0x0100 LB
 *   0x0200 RB           0x0400 GUIDE         0x0800 SHARE (Xbox One)
 *   0x1000 A            0x2000 B             0x4000 X
 *   0x8000 Y
 *
 * 注意：XInput 没有独立的 LT/RT 数字按钮（用模拟值判断），
 *       wButtons 共 16 bit，最高有效位为 bit 15 (Y)。
 *--------------------------------------------------------------------*/

#define XINPUT_BTN_COUNT  18

static const uint16_t xinput_button_map[XINPUT_BTN_COUNT] = {
    [0]  = BTN_DPAD_UP,
    [1]  = BTN_DPAD_DOWN,
    [2]  = BTN_DPAD_LEFT,
    [3]  = BTN_DPAD_RIGHT,
    [4]  = BTN_START,
    [5]  = BTN_SELECT,       // Back
    [6]  = BTN_LS,
    [7]  = BTN_RS,
    [8]  = BTN_LB,
    [9]  = BTN_RB,
    [10] = BTN_HOME,         // Guide
    [11] = BTN_MISC,         // Share (Xbox Series X|S)
    [12] = BTN_A,
    [13] = BTN_B,
    [14] = BTN_X,
    [15] = BTN_Y,
    [16] = BTN_LT,           // 左扳机数字 (模拟 ≥2/5 时置位)
    [17] = BTN_RT,           // 右扳机数字 (模拟 ≥2/5 时置位)
};

/*--------------------------------------------------------------------+
 * 转换函数
 *--------------------------------------------------------------------*/

/**
 * @brief 将 XInput 标准报告转换为归一化手柄状态
 *
 * @param pad   适配器归一化后的 XInput 数据（hidkit_xinput_pad_t）
 * @param out   输出：归一化状态（含 btn_map 指针，指向静态查表）
 * @return true 有效，false 输入为空
 */
static inline bool xinput_to_gamepad_state(const hidkit_xinput_pad_t *pad,
                                            gamepad_state_t *out)
{
    if (!pad || !out) return false;

    memset(out, 0, sizeof(*out));

    /* --- 摇杆：xinput 已是 int16_t，直接使用 ---
     * Xbox 360/One 摇杆范围约为 -32768 ~ 32767（实际略窄）。
     * 缩放因子: 除以 32768，使得 ±32767 映射到接近 ±INT16_MAX。
     *
     * 使用 INT16_MAX / 32768 ≈ 0.99997。保守起见直接用等比例：
     *   normalized = raw * INT16_MAX / 32767 （若 raw > 0）
     *              = raw * INT16_MAX / 32768 （若 raw < 0）
     * 简化：右移 scale 等价于除以 32768 缩放。
     * 实测 Xbox 摇杆物理范围远小于 ±32767，直接用 raw 已是安全的归一化值。
     */
    // hidkit：字段名随 hidkit_xinput_pad_t 归一化（原 sThumbLX/sThumbLY/...）
    out->ls_x = (int32_t)pad->lx;
    out->ls_y = -(int32_t)pad->ly;
    out->rs_x = (int32_t)pad->rx;
    out->rs_y = -(int32_t)pad->ry;

    /* --- 扳机：0~255 → 0~INT16_MAX --- */
    out->lt = (int32_t)pad->left_trigger  * (int32_t)(INT16_MAX / 255);
    out->rt = (int32_t)pad->right_trigger * (int32_t)(INT16_MAX / 255);

    /* --- 按钮：原生位布局直拷 + 扳机阈值 --- */
    out->buttons   = (uint32_t)pad->buttons;
    // 扳机行程 >= 2/5 (102/255) → 数字按钮按下
    if (pad->left_trigger  >= 102) out->buttons |= (1u << 16);
    if (pad->right_trigger >= 102) out->buttons |= (1u << 17);
    out->btn_map   = xinput_button_map;
    out->btn_count = XINPUT_BTN_COUNT;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_XINPUT_H
