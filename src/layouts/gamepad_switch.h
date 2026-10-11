/**
 * @file gamepad_switch.h
 * @brief Switch Pro / Joy-Con（057e:2009/2006/2007）解析
 *
 * 报文来源：USB 标准全量输入报告 0x30（49 字节）。这类手柄**主机握手前
 * 完全静默**，握手 / 使能 IMU（子命令 0x40）/ SPI 校准读取（子命令 0x10）
 * 都在宿主固件里完成；校准量经 hidkit_switchpro_set_calib() 注入后由
 * 本布局应用。库内不碰 USB，输出/初始化报文不在本层职责内。
 *
 * 报文布局（偏移含 Report ID）：
 *   [0]=0x30  [1]=timer  [2]=电量/连接（高半字节电量 2/4/6/8 + LSB 充电中）
 *   [3] 右组位：Y X B A SR SL R ZR（bit0..7）
 *   [4] 共享位：− + RS LS Home Capture Grip（bit0..6）
 *   [5] 左组位：下 上 右 左 SR SL L ZL（bit0..7）—— D-pad 是独立位不是 hat
 *   [6..8]/[9..11] 左/右摇杆 12bit 小端打包（0..4095，未校准）
 *   [12] 震动输入状态
 *   [13..48] 六轴 3 样本 × 12B（每样本 AccX/Y/Z + GyroX/Y/Z，Int16LE，间隔 5ms）
 *
 * 子命令应答（0x21，含 SPI 校准读回）与 USB 应答（0x81）由宿主握手模块
 * 消费；hidkit_report 将其标记已消费并跳过状态解析与事件输出。
 *
 * 按钮语义：沿用 DS5 的物理位置约定（面键"南"= BTN_A、"东"= BTN_B、
 * "北"= BTN_Y、"西"= BTN_X）—— Switch 实体 B 在南位、A 在东位，跨手柄
 * 同位置同 code。扳机无模拟量：ZL/ZR 数字位同时合成 lt/rt（0 或满量程）。
 *
 * 量程对齐（"对准量程"，见 include/hidkit.h 锚点）：
 *   陀螺仪：dps = (raw - origin) * 936 / (scale - origin)，再映射到 ±2000 dps；
 *   加速度：g = raw * 4 / (scale - origin)，再映射到 ±4 g（超出则截断）。
 *   加速度含重力；SPI 水平参考值不作为零偏扣除。换算参考 SDL Switch HIDAPI。
 *   Pro 经 gamepad_hid_dispatch 将六轴 (-Y, Z, -X) 对齐 DS5 坐标系；
 *   摇杆 Y 轴按 hidkit 的通用手柄约定取反（上推为正）；本文件先在原生轴上
 *   完成校准，Joy-Con 也使用同一输出约定。
 */

#ifndef GAMEPAD_SWITCH_H
#define GAMEPAD_SWITCH_H

#include "gamepad.h"
#include "hidkit_config.h"   // HIDKIT_HOT
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * Report 结构体（Report ID 之后的 48 字节 body）
 *--------------------------------------------------------------------*/

struct switchpro_input_report {
    uint8_t  timer;
    uint8_t  batconn;
    uint8_t  btn_r, btn_shared, btn_l;
    uint8_t  stick_l[3];
    uint8_t  stick_r[3];
    uint8_t  vibrator;
    int16_t  imu[18];   /* 3 样本 × [accX accY accZ gyroX gyroY gyroZ]，Int16LE */
} __attribute__((packed));

/*--------------------------------------------------------------------+
 * 常量与按钮表
 *--------------------------------------------------------------------*/

#define SW_VID            0x057E
#define SW_PID_PRO        0x2009
#define SW_PID_JOYCON_L   0x2006
#define SW_PID_JOYCON_R   0x2007
#define SW_RPT_ID         0x30
#define SW_STICK_MID      2048   /* 12bit 中心（未注入校准时的缺省值） */
#define SW_STICK_HALF     1024   /* 缺省半量程（校准到位前先给个能看的映射） */
#define SW_CAL_MAGIC0     0xB2   /* 用户校准区魔数（宿主侧核对，这里仅注释） */
#define SW_BTN_COUNT      24     /* 最高有效 bit 23（ZL）+ 1 */

/* bit → BTN_* 查表。0 = 该位不映射（Joy-Con 专属 SR/SL、Charging Grip 位，
 * Pro Controller 上恒为 0；hidkit_emit_key 对 code 0 丢弃）。
 * 位序 = 报文原生位序：bit0..7 右组、bit8..15 共享、bit16..23 左组。 */
static const uint16_t sw_button_map[SW_BTN_COUNT] = {
    [0]  = BTN_X,            // Y（西位）
    [1]  = BTN_Y,            // X（北位）
    [2]  = BTN_A,            // B（南位）
    [3]  = BTN_B,            // A（东位）
    [4]  = 0,                // SR（右，仅 Joy-Con）
    [5]  = 0,                // SL（右，仅 Joy-Con）
    [6]  = BTN_RB,           // R
    [7]  = BTN_RT,           // ZR（数字）
    [8]  = BTN_SELECT,       // −
    [9]  = BTN_START,        // +
    [10] = BTN_RS,           // 右摇杆按下
    [11] = BTN_LS,           // 左摇杆按下
    [12] = BTN_HOME,         // Home
    [13] = BTN_MISC,         // Capture
    [14] = 0,                // Charging Grip 标志位
    [15] = 0,
    [16] = BTN_DPAD_DOWN,    // 十字键 下
    [17] = BTN_DPAD_UP,      // 上
    [18] = BTN_DPAD_RIGHT,   // 右
    [19] = BTN_DPAD_LEFT,    // 左
    [20] = 0,                // SR（左，仅 Joy-Con）
    [21] = 0,                // SL（左，仅 Joy-Con）
    [22] = BTN_LB,           // L
    [23] = BTN_LT,           // ZL（数字）
};

/*--------------------------------------------------------------------+
 * 解析
 *--------------------------------------------------------------------*/

// VID/PID 匹配
static inline bool sw_match(uint16_t vid, uint16_t pid)
{
    return (vid == SW_VID &&
            (pid == SW_PID_PRO || pid == SW_PID_JOYCON_L || pid == SW_PID_JOYCON_R));
}

// 12bit 小端打包解包（3 字节 → 横/纵两个 12bit）
static inline void sw_stick_unpack(const uint8_t *d, uint16_t *h, uint16_t *v)
{
    *h = (uint16_t)(d[0] | ((d[1] & 0x0Fu) << 8));
    *v = (uint16_t)((d[1] >> 4) | (d[2] << 4));
}

// 单轴映射：raw(0..4095) → -32767..32767，正负向各自线性（校准量程不对称）
static inline int32_t sw_axis(uint16_t raw, uint16_t center,
                              uint16_t maxoff, uint16_t minoff)
{
    int32_t v = (int32_t)raw - (int32_t)center;
    int32_t r = (v >= 0) ? (int32_t)maxoff : (int32_t)minoff;
    if (r <= 0) r = 1;                       // 校准异常时防除零，输出钳到满量程
    int32_t out = (int32_t)((int64_t)v * 32767 / r);
    if (out >  32767) out =  32767;
    if (out < -32767) out = -32767;
    return out;
}

// 有符号四舍五入，再限幅。中间值用 64bit，避免传感器边界与零偏组合溢出。
static inline int16_t sw_motion_normalize(int64_t numerator, int64_t denominator)
{
    int64_t v = (numerator + (numerator >= 0 ? denominator / 2 : -denominator / 2)) /
                denominator;
    if (v >  32767) v =  32767;
    if (v < -32767) v = -32767;
    return (int16_t)v;
}

static inline int16_t sw_gyro_axis(int32_t raw, int32_t origin, int32_t scale)
{
    int32_t divisor = scale - origin;
    if (scale <= 0 || divisor <= 0) {
        origin = 0;
        divisor = 13371; // 936 / 13371 ≈ 0.070 dps/count
    }
    return sw_motion_normalize((int64_t)(raw - origin) * 936 * 32767,
                                (int64_t)divisor * HIDKIT_MOTION_GYRO_FULL_SCALE_DPS);
}

static inline int16_t sw_accel_axis(int32_t raw, int32_t origin, int32_t scale)
{
    int32_t divisor = scale - origin;
    if (scale <= 0 || divisor <= 0) divisor = 16384;
    // acc_origin 只在分母里；直接扣 origin 或平放参考值会错误地移除重力。
    return sw_motion_normalize((int64_t)raw * 4000 * 32767,
                                (int64_t)divisor * HIDKIT_MOTION_ACCEL_FULL_SCALE_MG);
}

// 解析 raw report → gamepad_state_t（除校准读取外无状态，每帧独立）。
// 校准按槽位读取：gamepad_hid_dispatch() 调进来前已置好 s_parse_slot
//（gamepad.c），经 switch_calib_of() 拿本槽位的注入数据（可能为 NULL =
// 尚未注入，用缺省映射 —— 0x30 流先出来，校准到位后自动变准）。
static bool HIDKIT_HOT(sw_parse)(const uint8_t *report, uint16_t len,
                                 gamepad_state_t *out)
{
    if (!report || !out) return false;

    if (len < 1) return false;
    // parse 返回 true 必须填好输入状态；控制应答由上层单独消费。
    if (report[0] != SW_RPT_ID) return false;
    uint16_t body_len = len - 1;
    if (body_len < sizeof(struct switchpro_input_report)) return false;

    /* 报文缓冲无对齐保证，先 memcpy 再访问（同 DS5 的做法） */
    struct switchpro_input_report rpt_buf;
    memcpy(&rpt_buf, report + 1, sizeof(rpt_buf));
    const struct switchpro_input_report *rpt = &rpt_buf;

    memset(out, 0, sizeof(*out));

    const hidkit_switchpro_calib_t *cal = switch_calib_of(s_parse_slot);
    const bool have_stick = cal && (cal->valid & HIDKIT_SWITCHPRO_CAL_STICKS);
    const bool have_motion = cal && (cal->valid & HIDKIT_SWITCHPRO_CAL_MOTION);

    // --- 摇杆：校准中心/量程（未注入时缺省中心 2048、半量程 1024）---
    uint16_t lx, ly, rx, ry;
    sw_stick_unpack(rpt->stick_l, &lx, &ly);
    sw_stick_unpack(rpt->stick_r, &rx, &ry);
    out->ls_x = sw_axis(lx, have_stick ? cal->ls_xc    : SW_STICK_MID,
                           have_stick ? cal->ls_xmax : SW_STICK_HALF,
                           have_stick ? cal->ls_xmin : SW_STICK_HALF);
    // Switch 原生 Y 轴与 DS5/通用手柄的 Y 方向相反；校准仍按原始方向选取
    // max/min，最后只翻转归一化结果，避免不对称量程时交换校准边界。
    out->ls_y = -sw_axis(ly, have_stick ? cal->ls_yc    : SW_STICK_MID,
                            have_stick ? cal->ls_ymax : SW_STICK_HALF,
                            have_stick ? cal->ls_ymin : SW_STICK_HALF);
    out->rs_x = sw_axis(rx, have_stick ? cal->rs_xc    : SW_STICK_MID,
                           have_stick ? cal->rs_xmax : SW_STICK_HALF,
                           have_stick ? cal->rs_xmin : SW_STICK_HALF);
    out->rs_y = -sw_axis(ry, have_stick ? cal->rs_yc    : SW_STICK_MID,
                            have_stick ? cal->rs_ymax : SW_STICK_HALF,
                            have_stick ? cal->rs_ymin : SW_STICK_HALF);

    // --- 扳机：无模拟量，ZL/ZR 数字位合成 0 / 满量程 ---
    out->lt = (rpt->btn_l & 0x80) ? 32767 : 0;
    out->rt = (rpt->btn_r & 0x80) ? 32767 : 0;

    // --- 六轴：取报文内最新样本（每包 3 样本 × 5ms，出口每报文一帧）---
    const int16_t *s = rpt->imu + 12;    /* 样本 3 的 accX 起 */
    const int16_t *ao = have_motion ? cal->acc_origin  : NULL;
    const int16_t *go = have_motion ? cal->gyro_origin : NULL;
    const int16_t *as = have_motion ? cal->acc_scale : NULL;
    const int16_t *gs = have_motion ? cal->gyro_scale : NULL;
    out->gyro_x  = sw_gyro_axis(s[3], go ? go[0] : 0, gs ? gs[0] : 13371);
    out->gyro_y  = sw_gyro_axis(s[4], go ? go[1] : 0, gs ? gs[1] : 13371);
    out->gyro_z  = sw_gyro_axis(s[5], go ? go[2] : 0, gs ? gs[2] : 13371);
    out->accel_x = sw_accel_axis(s[0], ao ? ao[0] : 0, as ? as[0] : 16384);
    out->accel_y = sw_accel_axis(s[1], ao ? ao[1] : 0, as ? as[1] : 16384);
    out->accel_z = sw_accel_axis(s[2], ao ? ao[2] : 0, as ? as[2] : 16384);
    out->has_motion = 1;

    // --- 按键：原生位布局一次拼装（bit0..7 右组 / 8..15 共享 / 16..23 左组）---
    out->buttons = (uint32_t)rpt->btn_r |
                   ((uint32_t)rpt->btn_shared << 8) |
                   ((uint32_t)rpt->btn_l << 16);

    out->btn_map   = sw_button_map;
    out->btn_count = SW_BTN_COUNT;

    return true;
}

#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_SWITCH_H
