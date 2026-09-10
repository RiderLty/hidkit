/**
 * @file gamepad_azeron.h
 * @brief Azeron 左手键盘摇杆专用控制器解析
 *
 */

#ifndef GAMEPAD_AZERON_H
#define GAMEPAD_AZERON_H

#include "gamepad.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * Report 结构体
 *--------------------------------------------------------------------*/
struct azeron_input_report {
    uint32_t head;      // 0x00
    union {
        uint8_t ls_bytes[3];           // 原始3字节（小端序）
        struct {
            uint32_t ls_padding : 4;           // 低12位 → 水平轴（Y）
            uint32_t ls_x : 10;           // 低12位 → 水平轴（Y）
            uint32_t ls_y : 10;           // 高12位 → 垂直轴（X）
        } __attribute__((packed));
    };
    uint8_t  padding_1;
    uint32_t padding_2;
} __attribute__((packed));
/*--------------------------------------------------------------------+
 * 常量
 *--------------------------------------------------------------------*/

#define AZERON_VID          0x16D0
#define AZERON_PID          0x13EA

/*--------------------------------------------------------------------+
 * 控制器专用解析
 *--------------------------------------------------------------------*/

// VID/PID 匹配
static inline bool azeron_match(uint16_t vid, uint16_t pid)
{
    // hidkit：去掉应用侧 DEBUG 依赖（库内无日志通道）
    return (vid == AZERON_VID && pid == AZERON_PID);
}

// 解析 raw report → gamepad_state_t（无状态，每帧独立）
static inline bool azeron_parse(const uint8_t *report, uint16_t len,
                                gamepad_state_t *out)
{
    if (!report || !out) return false;
    if (len != sizeof(struct azeron_input_report)) return false;
    // hidkit：不再把报文缓冲强转成 packed 结构体指针（无对齐保证，Cortex-M0
    // 会 HardFault），先 memcpy 到局部变量再访问
    struct azeron_input_report rpt_buf;
    memcpy(&rpt_buf, report, sizeof(rpt_buf));
    const struct azeron_input_report *rpt = &rpt_buf;
    memset(out, 0, sizeof(*out));
    out->ls_x = ((int32_t)rpt->ls_x - 512 ) << 6 ; // 中心化
    out->ls_y = ((int32_t)rpt->ls_y - 512) << 6 ; // 中心化
    return true;
}

#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_AZERON_H
