/**
 * @file gamepad_ds5.h
 * @brief DS5 (DualSense) 控制器解析
 *
 * 来源: https://github.com/RiderLty/pico_ds5_ctrl/blob/main/ds.h
 * HID Report ID = 0x01, 跳过首字节后 memcpy 出 dualsense_input_report
 * （报文缓冲无对齐保证，不能直接 cast 成 packed 结构体指针）
 */

#ifndef GAMEPAD_DS5_H
#define GAMEPAD_DS5_H

#include "gamepad.h"
#include "hidkit_config.h"   // HIDKIT_HOT
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * Report 结构体
 *--------------------------------------------------------------------*/

struct touch_point {
    uint8_t contact;
    union {
        uint8_t raw[3];
        struct {
            uint32_t x : 12;
            uint32_t y : 12;
        } __attribute__((packed));
    };
} __attribute__((packed));

union ds_buttons {
    uint32_t raw;
    struct {
        uint32_t dpad      : 4;  // bit 0-3  方向键枚举
        uint32_t x         : 1;  // bit 4    Square
        uint32_t a         : 1;  // bit 5    Cross
        uint32_t b         : 1;  // bit 6    Circle
        uint32_t y         : 1;  // bit 7    Triangle
        uint32_t lb        : 1;  // bit 8    L1
        uint32_t rb        : 1;  // bit 9    R1
        uint32_t lt        : 1;  // bit 10   L2 数字
        uint32_t rt        : 1;  // bit 11   R2 数字
        uint32_t back      : 1;  // bit 12   Create
        uint32_t start     : 1;  // bit 13   Options
        uint32_t ls        : 1;  // bit 14   L3
        uint32_t rs        : 1;  // bit 15   R3
        uint32_t ps        : 1;  // bit 16   PS
        uint32_t touchpad  : 1;  // bit 17   Pad Click
        uint32_t mute      : 1;  // bit 18   Mic
        uint32_t reserved  : 13; // bit 19-31
    } __attribute__((packed));
};

struct dualsense_input_report {
    uint8_t ls_x, ls_y;
    uint8_t rs_x, rs_y;
    uint8_t lt, rt;
    uint8_t seq_number;
    union ds_buttons buttons;
    uint32_t reserved;
    uint16_t gyro_x, gyro_y, gyro_z;
    uint16_t accel_x, accel_y, accel_z;
    uint32_t sensor_timestamp;
    uint8_t reserved2;
    struct touch_point points_1;
    struct touch_point points_2;
    uint8_t reserved3[12];
    uint8_t status;
    uint8_t reserved4[10];
} __attribute__((packed));

/*--------------------------------------------------------------------+
 * 常量
 *--------------------------------------------------------------------*/

#define DS5_VID          0x054C
#define DS5_PID          0x0CE6
#define DS5_RPT_ID       0x01
#define DS5_STICK_MID    128
#define DS5_STICK_RANGE  127
#define DS5_TRIG_MAX     255

// dpad 枚举 → 方向位 nibble 的查找表（64-bit = 16 nibble）
// nibble: bit0=UP, bit1=RIGHT, bit2=DOWN, bit3=LEFT (与 buttons bit 0-3 对齐)
// dpad: 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW, 8/15=释放
#define DS5_DPAD_LUT  0x098C46231ULL

/*--------------------------------------------------------------------+
 * DS5 按钮位 → 按钮类型 查表（static，编译期嵌入）
 *
 * 用法:
 *   uint16_t btn = ds5_button_map[bit];  // bit=0..31
 *   if (btn == BTN_A) { ... }
 *--------------------------------------------------------------------*/

#define DS5_BTN_COUNT  19   // 最高有效 bit 18 + 1

static const uint16_t ds5_button_map[DS5_BTN_COUNT] = {
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
    [17] = BTN_MISC,      // Touchpad
    [18] = BTN_EXTRA_1,   // Mute
};

/*--------------------------------------------------------------------+
 * 控制器专用解析
 *--------------------------------------------------------------------*/

// VID/PID 匹配
static inline bool ds5_match(uint16_t vid, uint16_t pid)
{
    return (vid == DS5_VID && pid == DS5_PID);
}

// 解析 raw report → gamepad_state_t（无状态，每帧独立）
// 每报文（经 gamepad.c 的函数指针调进来）。它是 static inline，但地址被
// gp_lookup 取走，必然发射出独立函数体 —— 那个副本要跟着进 RAM，
// 否则函数指针调用点仍落在 flash 上
static inline bool HIDKIT_HOT(ds5_parse)(const uint8_t *report, uint16_t len,
                              gamepad_state_t *out)
{
    if (!report || !out) return false;

    // 跳过 Report ID 字节
    if (len < 1 || report[0] != DS5_RPT_ID) return false;
    uint16_t body_len = len - 1;
    if (body_len < sizeof(struct dualsense_input_report)) return false;

    // hidkit：不再把报文缓冲强转成 packed 结构体指针（无对齐保证，Cortex-M0
    // 会 HardFault），先 memcpy 到局部变量再访问
    struct dualsense_input_report rpt_buf;
    memcpy(&rpt_buf, report + 1, sizeof(rpt_buf));
    const struct dualsense_input_report *rpt = &rpt_buf;

    memset(out, 0, sizeof(*out));

    // --- 摇杆：中心化 → 缩放 ---
    // 死区由上层统一处理（圆形死区，用户可配置），
    // 此处不做逐轴死区，避免方形截断破坏方向（尤其对极坐标映射致命）
    int32_t lx = (int32_t)rpt->ls_x - DS5_STICK_MID;
    int32_t ly = (int32_t)rpt->ls_y - DS5_STICK_MID;
    int32_t rx = (int32_t)rpt->rs_x - DS5_STICK_MID;
    int32_t ry = (int32_t)rpt->rs_y - DS5_STICK_MID;

    // 摇杆原始 8bit(0~255)，中心化后 -128~+127；左移 8 位即映射到 int16 量程
    // （-32768 ~ +32512）。比 ×258 更便宜，负向也不再越出 int16。
    out->ls_x = lx << 8;
    out->ls_y = ly << 8;
    out->rs_x = rx << 8;
    out->rs_y = ry << 8;

    // --- 模拟扳机 ---
    out->lt = ((int32_t)rpt->lt * (int32_t)(INT16_MAX / DS5_TRIG_MAX));
    out->rt = ((int32_t)rpt->rt * (int32_t)(INT16_MAX / DS5_TRIG_MAX));

    // --- 按键：原生布局一次拷贝 ---
    out->buttons = rpt->buttons.raw;

    // dpad: 清除枚举 nibble (bit 0-3)，替换为方向位
    out->buttons &= ~0x0Fu;
    uint8_t dpad_local = (uint8_t)(DS5_DPAD_LUT >> (rpt->buttons.dpad * 4)) & 0x0f;
    out->buttons |= dpad_local;

    // --- 查表指针 + 长度 ---
    out->btn_map   = ds5_button_map;
    out->btn_count = DS5_BTN_COUNT;

    return true;
}

#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_DS5_H
