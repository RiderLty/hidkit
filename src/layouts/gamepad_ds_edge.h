/**
 * @file gamepad_ds_edge.h
 * @brief DS Edge (DualSense Edge) 控制器解析
 *
 * 来源: Linux 内核 drivers/hid/hid-playstation.c（DS_EDGE_BUTTONS_* 定义）
 * Edge 复用 DS5 的输入报文（Report ID = 0x01，USB 64 字节），报文结构完全一致；
 * 差别只有两处:
 *   1. VID/PID: USB 与蓝牙同为 0x0DF2（标准 DS5 为 0x0CE6）
 *   2. 附加按键: buttons[2]（32bit 按键字的 bit 20-23，DS5 布局中的 reserved 区）
 *      bit20=FN1, bit21=FN2, bit22=左背键, bit23=右背键
 *
 * 因此解析直接复用 ds5_parse，仅在末尾换掉按钮查表指针。
 * 物理命名以 Linux 内核为准（FN1/FN2 与左右背键），真机可用 tests/linux/hidkit_probe
 * 实测确认每个 bit 对应的实际按键。
 */

#ifndef GAMEPAD_DS_EDGE_H
#define GAMEPAD_DS_EDGE_H

#include "gamepad.h"
#include "gamepad_ds5.h"     // 复用 dualsense_input_report 与 ds5_parse
#include "hidkit_config.h"   // HIDKIT_HOT

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * 常量
 *--------------------------------------------------------------------*/

#define DS_EDGE_VID          0x054C
#define DS_EDGE_PID          0x0DF2   // USB 与蓝牙同 PID（内核 hid-ids.h 只此一个）
#define DS_EDGE_BTN_COUNT    24       // 最高有效 bit 23 + 1

// 附加按键位（32bit 按键字内，buttons[2] 的 bit4-7）
#define DS_EDGE_BTN_FN1       (1u << 20)
#define DS_EDGE_BTN_FN2       (1u << 21)
#define DS_EDGE_BTN_PADDLE_L  (1u << 22)
#define DS_EDGE_BTN_PADDLE_R  (1u << 23)

/*--------------------------------------------------------------------+
 * DS Edge 按钮位 → 按钮类型 查表
 *
 * bit 0-18 与 ds5_button_map 完全一致（报文布局相同），bit 20-23 为
 * Edge 新增按键。内核将它们映射为 BTN_TRIGGER_HAPPY1..4，这里归入
 * hidkit 的 BTN_EXTRA_* 段。
 *--------------------------------------------------------------------*/

static const uint16_t ds_edge_button_map[DS_EDGE_BTN_COUNT] = {
    [0]  = BTN_DPAD_UP,    // dpad 方向位 (LUT 填充)
    [1]  = BTN_DPAD_RIGHT,
    [2]  = BTN_DPAD_DOWN,
    [3]  = BTN_DPAD_LEFT,
    [4]  = BTN_X,           // Square
    [5]  = BTN_A,           // Cross
    [6]  = BTN_B,           // Circle
    [7]  = BTN_Y,           // Triangle
    [8]  = BTN_LB,          // L1
    [9]  = BTN_RB,          // R1
    [10] = BTN_LT,          // L2 数字
    [11] = BTN_RT,          // R2 数字
    [12] = BTN_SELECT,      // Create
    [13] = BTN_START,       // Options
    [14] = BTN_LS,          // L3
    [15] = BTN_RS,          // R3
    [16] = BTN_HOME,        // PS
    [17] = BTN_MISC,        // Touchpad
    [18] = BTN_EXTRA_1,     // Mute
    // bit 19 保留
    [20] = BTN_EXTRA_2,     // FN1（内核 DS_EDGE_BUTTONS_FN1）
    [21] = BTN_EXTRA_3,     // FN2（内核 DS_EDGE_BUTTONS_FN2）
    [22] = BTN_EXTRA_4,     // 左背键 Paddle L
    [23] = BTN_EXTRA_5,     // 右背键 Paddle R
};

/*--------------------------------------------------------------------+
 * 控制器专用解析
 *--------------------------------------------------------------------*/

// VID/PID 匹配
static inline bool ds_edge_match(uint16_t vid, uint16_t pid)
{
    return (vid == DS_EDGE_VID && pid == DS_EDGE_PID);
}

// 解析 raw report → gamepad_state_t（无状态，每帧独立）
// 报文结构与 DS5 完全一致（含 bit 20-23 的透传：ds5_parse 原样拷贝整个
// 32bit 按键字，Edge 的附加位天然包含在内），故直接复用 ds5_parse，
// 换成本布局的查表指针即可。
static inline bool HIDKIT_HOT(ds_edge_parse)(const uint8_t *report, uint16_t len,
                              gamepad_state_t *out)
{
    if (!ds5_parse(report, len, out)) return false;
    out->btn_map   = ds_edge_button_map;
    out->btn_count = DS_EDGE_BTN_COUNT;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_DS_EDGE_H
