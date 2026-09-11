/**
 * @file gamepad.c
 * @brief 游戏手柄调度层 —— 按 VID/PID 注册对应解析器，dispatch 时函数指针调用
 *
 * 架构：
 *   hidkit_mount()（由宿主 USB 栈的挂载回调驱动）
 *     → gp_parse_fn = ds5_match()? ds5_parse : azeron_match()? azeron_parse : ...
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

// ── 已支持手柄 ──
#include "layouts/gamepad_ds5.h"
#include "layouts/gamepad_azeron.h"
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

/*--------------------------------------------------------------------+
 * 公共 API 实现
 *--------------------------------------------------------------------*/

// ── 按 VID/PID 查解析器（纯函数，不碰槽位状态）──
// 添加新手柄：只改这一处（+ 上面的 #include）
static gp_parse_fn gp_lookup(uint16_t vid, uint16_t pid)
{
    if (ds5_match(vid, pid))    return ds5_parse;
    if (azeron_match(vid, pid)) return azeron_parse;
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
    return gp->parse(report, len, out_state);
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
