/**
 * @file gamepad.c
 * @brief 游戏手柄调度层 —— 按 VID/PID 注册对应解析器，dispatch 时函数指针调用
 *
 * 架构：
 *   hidkit_mount()（由宿主 USB 栈的挂载回调驱动）
 *     → gp_parse_fn = ds5_match()? ds5_parse : ds_edge_match()? ds_edge_parse
 *                    : azeron_match()? azeron_parse : ...
 *   hidkit_report()
 *     → gp->parse(report, len, &state)   // 函数指针，不 switch
 *     → 上层据此产出事件（按键边沿 → key 事件、轴 → gamepad_abs）
 *
 * 添加新手柄：
 *   1. 新建 src/layouts/gamepad_xxx.h
 *   2. 在本文件 #include 并 mount 中加一行 else if
 *   dispatch / umount 无需改动
 */

#include "gamepad.h"
#include "hidkit_config.h"  // HIDKIT_MAX_SLOTS
#include <string.h>         // memset

// ── 前置声明：switch 布局的 parse 需要按槽位读校准。存储与定义在本文件
//    下方（布局头在上方 include，parse 函数体在此 TU 内编译，可见声明即可）
static int8_t s_parse_slot;   // 正在解析的槽位（gamepad_hid_dispatch 置位）
static const hidkit_switchpro_calib_t *switch_calib_of(int8_t slot);

// ── 已支持手柄 ──
#include "layouts/gamepad_ds5.h"
#include "layouts/gamepad_ds_edge.h"   // 复用 ds5 报文布局，仅 PID 与按钮表不同
#include "layouts/gamepad_azeron.h"
#include "layouts/gamepad_switch.h"    // Switch Pro / Joy-Con（需宿主握手，见头文件注释）
/*--------------------------------------------------------------------+
 * 解析函数指针类型
 *--------------------------------------------------------------------*/

typedef bool (*gp_parse_fn)(const uint8_t *report, uint16_t len,
                             gamepad_state_t *out);

/*--------------------------------------------------------------------+
 * 全局状态
 *--------------------------------------------------------------------*/

typedef struct {
    gp_parse_fn parse;   // 解析函数指针
    bool        active;
    uint16_t    vid, pid;  // 供 valid_gp_pid_vid 核对
} gamepad_info_t;

// hidkit：槽位数改用 HIDKIT_MAX_SLOTS（hidkit_config.h，可 -D 覆盖）
static gamepad_info_t g_gp[HIDKIT_MAX_SLOTS];

// ── Switch Pro 校准存储（按槽位；注入时机晚于报文流开始，见 hidkit.h 注释）──
static hidkit_switchpro_calib_t s_swcal[HIDKIT_MAX_SLOTS];
static uint8_t s_swcal_valid[HIDKIT_MAX_SLOTS];

const hidkit_switchpro_calib_t *switch_calib_of(int8_t slot)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS || !s_swcal_valid[slot])
        return NULL;
    return &s_swcal[slot];
}

bool hidkit_switchpro_set_calib(int8_t slot, const hidkit_switchpro_calib_t *cal)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS || !cal || !g_gp[slot].active)
        return false;
    if (!sw_match(g_gp[slot].vid, g_gp[slot].pid))
        return false;   // 校准只对 Switch 布局有意义

    // 合理性：12bit 中心越界 / 满偏为 0 的摇杆数据不可用
    if (cal->valid & HIDKIT_SWITCHPRO_CAL_STICKS) {
        if (cal->ls_xc > 4095 || cal->ls_yc > 4095 ||
            cal->rs_xc > 4095 || cal->rs_yc > 4095 ||
            cal->ls_xmax == 0 || cal->ls_xmin == 0 ||
            cal->ls_ymax == 0 || cal->ls_ymin == 0 ||
            cal->rs_xmax == 0 || cal->rs_xmin == 0 ||
            cal->rs_ymax == 0 || cal->rs_ymin == 0)
            return false;
    }

    s_swcal[slot] = *cal;
    s_swcal_valid[slot] = (uint8_t)(cal->valid & (HIDKIT_SWITCHPRO_CAL_STICKS |
                                                  HIDKIT_SWITCHPRO_CAL_MOTION));
    return true;
}

/*--------------------------------------------------------------------+
 * 公共 API 实现
 *--------------------------------------------------------------------*/

// ── 按 VID/PID 查解析器（纯函数，不碰槽位状态）──
// 添加新手柄：只改这一处（+ 上面的 #include）
static gp_parse_fn gp_lookup(uint16_t vid, uint16_t pid)
{
    if (ds5_match(vid, pid))    return ds5_parse;
    if (ds_edge_match(vid, pid)) return ds_edge_parse;
    if (azeron_match(vid, pid)) return azeron_parse;
    if (sw_match(vid, pid))      return sw_parse;
    return NULL;   // 未知手柄
}

bool gamepad_hid_match(uint16_t vid, uint16_t pid)
{
    return gp_lookup(vid, pid) != NULL;
}

bool gamepad_hid_mount(int8_t slot, uint16_t vid, uint16_t pid,
                       const uint8_t *desc_report, uint16_t desc_len)
{
    (void)desc_report;
    (void)desc_len;

    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS)
        return false;

    gamepad_info_t *gp = &g_gp[slot];
    memset(gp, 0, sizeof(*gp));
    s_swcal_valid[slot] = 0;    // 新设备接管：上一台的校准必须失效

    gp_parse_fn parse = gp_lookup(vid, pid);
    if (!parse) return false;   // 未知手柄（槽位保持清零 = 未激活）

    gp->parse = parse;
    gp->active = true;
    gp->vid = vid;
    gp->pid = pid;
    return true;
}

void gamepad_hid_umount(int8_t slot)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS)
        return;
    memset(&g_gp[slot], 0, sizeof(gamepad_info_t));
    s_swcal_valid[slot] = 0;
}

// 每报文（SLOT_GAMEPAD 分支）。与键盘/鼠标/NKRO 的 dispatch 同级，标注也一致；
// 它是热路径上唯一用函数指针调度的一环，指向下面的 ds5_parse / azeron_parse
bool HIDKIT_HOT(gamepad_hid_dispatch)(int8_t slot,
                          const uint8_t *report, uint16_t len,
                          gamepad_state_t *out_state)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS || !report || !out_state)
        return false;

    const gamepad_info_t *gp = &g_gp[slot];
    if (!gp->active) return false;

    // ── 函数指针调度（固定，永不改）──
    // s_parse_slot：布局 parse 无槽位参数（保持无状态契约），但 Switch 校准
    // 按槽位注入 —— dispatch 是 parse 的唯一调用点且单线程（tuh_task 上下文），
    // 先置后用是安全的
    s_parse_slot = slot;
    bool parsed = gp->parse(report, len, out_state);
    if (parsed && gp->vid == SW_VID && gp->pid == SW_PID_PRO) {
        // Pro 的原生坐标系对齐 DS5；校准必须先在传感器原生轴上应用。
        // SDL Switch HIDAPI SendSensorUpdate 同样采用 (-Y, Z, -X)。
        int16_t gx = out_state->gyro_x, gy = out_state->gyro_y, gz = out_state->gyro_z;
        int16_t ax = out_state->accel_x, ay = out_state->accel_y, az = out_state->accel_z;
        out_state->gyro_x = -gy;  out_state->gyro_y = gz;  out_state->gyro_z = -gx;
        out_state->accel_x = -ay; out_state->accel_y = az; out_state->accel_z = -ax;
    }
    return parsed;
}

bool gamepad_hid_is_reply(int8_t slot, const uint8_t *report, uint16_t len)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS || !report || !len)
        return false;
    const gamepad_info_t *gp = &g_gp[slot];
    return gp->active && sw_match(gp->vid, gp->pid) &&
           (report[0] == 0x21 || report[0] == 0x81);
}

bool gamepad_is_active(int8_t slot)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return false;
    return g_gp[slot].active;
}

bool valid_gp_pid_vid(int8_t slot, uint16_t vid, uint16_t pid)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return false;
    const gamepad_info_t *gp = &g_gp[slot];
    return gp->active && gp->vid == vid && gp->pid == pid;
}
