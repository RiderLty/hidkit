/*
 * hidkit 出口层：槽位管理 + 入口分发 + 事件出口。
 *
 * 这一层是唯一"知道设备类型怎么判定"的地方：
 *   - proto == MOUSE  → 解析鼠标报告描述符（固定格式报文兜底）
 *   - proto == KEYBOARD → boot 协议固定格式（无需描述符）
 *   - 其它（proto == 0）→ 先按 VID:PID 匹配已知手柄布局；不中再尝试 NKRO
 *                        键盘描述符；都不中则返回"未消费"交宿主处理
 *
 * 解析层（hid_parser / hid_dispatch / gamepad）不依赖任何平台 API，
 * 本层同样如此 —— 时间源与热路径标注都是可选宏。
 */

#include "hidkit.h"
#include "hidkit_config.h"   // HIDKIT_HOT / 容量宏
#include "hidkit_internal.h"
#include "hidkit_hooks.h"
#include "hidkit_debug.h"

#include "hid_parser.h"
#include "hid_dispatch.h"
#include "gamepad.h"

#include <string.h>

/*--------------------------------------------------------------------+
 * 槽位表
 *--------------------------------------------------------------------*/

typedef enum {
    SLOT_FREE = 0,
    SLOT_MOUSE,
    SLOT_KEYBOARD,
    SLOT_GAMEPAD,
    SLOT_NKRO,
} slot_kind_t;

typedef struct {
    slot_kind_t kind;
    uint16_t vid, pid;
    uint8_t  dev_addr, itf, proto;
    uint32_t last_buttons;                 /* 手柄按键边沿用 */
    const uint16_t *btn_map;               /* 手柄布局表（指向静态表，卸载时补发松开要用） */
    uint8_t  btn_count;
    uint32_t seq;                          /* 分配序号：EVICT_IDLE 的退化判据 */
    bool     logged_unconsumed;             /* 该槽位的「报文未消费」只报一次 */
#if defined(HIDKIT_TICK_MS)
    uint32_t last_tick;                    /* 最近一次收到报文的时刻 */
#endif
} hidkit_slot_t;

static hidkit_slot_t s_slot[HIDKIT_MAX_SLOTS];
static uint32_t s_alloc_seq;
static uint8_t s_kb_count, s_mouse_count;

/* 各解析器的设备状态（按槽位分槽，与解析模块一一对应） */
static hid_mouse_dev_t  s_mouse[HIDKIT_MAX_SLOTS];
static hid_nkro_dev_t   s_nkro[HIDKIT_MAX_SLOTS];

/*--------------------------------------------------------------------+
 * 事件出口（解析层通过 hidkit_internal.h 调用）
 *--------------------------------------------------------------------*/

void hidkit_emit_key(int8_t slot, uint16_t code, bool pressed)
{
    uint16_t c = code;
    bool p = pressed;
    if (!hidkit_hook_key(&c, &p)) return;   /* 钩子可改写或吞掉 */
    if (c == 0) return;                     /* 0 = 无效/空槽位约定 */
    hidkit_input_key(slot, c, p);
}

void hidkit_emit_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    int32_t x = dx, y = dy, w = wheel;
    if (!hidkit_hook_mouse_abs(&x, &y, &w)) return;
    if (x == 0 && y == 0 && w == 0) return;
    hidkit_input_mouse_abs(slot, x, y, w);
}

void hidkit_emit_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                             int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt)
{
    if (!hidkit_hook_gamepad_abs(&ls_x, &ls_y, &rs_x, &rs_y, &lt, &rt)) return;
    hidkit_input_gamepad_abs(slot, ls_x, ls_y, rs_x, rs_y, lt, rt);
}

// 每份报文的第一个调用（hidkit_report 进来就查）。跨 TU 无法内联，所以在热
// 路径标注这件事上它必须一起标 —— 只标下层 dispatch 而把这一句留在 flash，
// 整条链照样会在 flash 写窗口里卡在第一次取指上
bool HIDKIT_HOT(hidkit_slot_alive)(int8_t slot)
{
    return slot >= 0 && slot < (int8_t)HIDKIT_MAX_SLOTS && s_slot[slot].kind != SLOT_FREE;
}

/*--------------------------------------------------------------------+
 * 槽位分配
 *--------------------------------------------------------------------*/

static void slot_clear(int8_t slot)
{
    memset(&s_slot[slot], 0, sizeof(s_slot[slot]));
    memset(&s_mouse[slot], 0, sizeof(s_mouse[slot]));
    memset(&s_nkro[slot], 0, sizeof(s_nkro[slot]));
}

static inline bool slot_is_kb(slot_kind_t k)
{
    return k == SLOT_KEYBOARD || k == SLOT_NKRO;
}

/* 释放一个槽位：先补发"全部抬起"，再清状态（卸载与挤出共用） */
static void slot_release(int8_t slot)
{
    slot_kind_t const kind = s_slot[slot].kind;

    if (kind == SLOT_NKRO) {
        hid_nkro_release_all(slot, &s_nkro[slot]);
    }
    hid_dispatch_reset(slot);            /* boot 键鼠的补发松开 */

    if (kind == SLOT_GAMEPAD) {
        /* 手柄按键补发松开（按布局表查回 BTN_* 再走统一 key 出口），轴归零 */
        const uint16_t *map = s_slot[slot].btn_map;
        uint8_t count = s_slot[slot].btn_count;
        for (uint8_t i = 0; map && i < count && i < 32; i++) {
            if (s_slot[slot].last_buttons & (1u << i)) {
                uint8_t code = (uint8_t)(map[i] & 0xFF);
                hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_GAMEPAD | code), false);
            }
        }
        hidkit_emit_gamepad_abs(slot, 0, 0, 0, 0, 0, 0);
    }

    if (slot_is_kb(kind) && s_kb_count) s_kb_count--;
    if (kind == SLOT_MOUSE && s_mouse_count) s_mouse_count--;

    slot_clear(slot);
}

/* 找一个空槽；没有空槽时按 HIDKIT_OVERFLOW 决定挤出还是放弃 */
static int8_t slot_alloc(void)
{
    for (int8_t i = 0; i < (int8_t)HIDKIT_MAX_SLOTS; i++) {
        if (s_slot[i].kind == SLOT_FREE) return i;
    }
#if HIDKIT_OVERFLOW == HIDKIT_OVERFLOW_EVICT_IDLE
    /* 挤掉"最久没有报文"的槽位；没有时间源时退化为分配序号最小的（最老） */
    int8_t victim = -1;
    uint32_t best = 0xFFFFFFFFu;
    for (int8_t i = 0; i < (int8_t)HIDKIT_MAX_SLOTS; i++) {
        uint32_t metric;
#if defined(HIDKIT_TICK_MS)
        metric = s_slot[i].last_tick;
#else
        metric = s_slot[i].seq;
#endif
        if (metric < best) {
            best = metric;
            victim = i;
        }
    }
    if (victim >= 0) {
        HIDKIT_LOG("hidkit: evict slot %d (%04x:%04x)\n",
                   victim, s_slot[victim].vid, s_slot[victim].pid);
        slot_release(victim);   /* 补发松开，避免上层按键残留 */
        return victim;
    }
#endif
    return HIDKIT_ERR_NO_SLOT;
}

static void slot_touch(int8_t slot)
{
    if (!hidkit_slot_alive(slot)) return;
#if defined(HIDKIT_TICK_MS)
    s_slot[slot].last_tick = (uint32_t)HIDKIT_TICK_MS();
#endif
}

/*--------------------------------------------------------------------+
 * 入口
 *--------------------------------------------------------------------*/

void hidkit_init(void)
{
    for (int8_t i = 0; i < (int8_t)HIDKIT_MAX_SLOTS; i++) slot_clear(i);
    s_alloc_seq = 0;
    s_kb_count = 0;
    s_mouse_count = 0;
}

int8_t hidkit_mount(const hidkit_dev_info_t *dev)
{
    if (!dev) return HIDKIT_UNHANDLED;
    const bool has_desc = (dev->report_desc && dev->report_desc_len > 0);

    /* --- 先判定"本库是否认识这个设备"（不认识就不占槽位）--- */
    enum { KIND_NONE, KIND_MOUSE, KIND_KB, KIND_GAMEPAD, KIND_NKRO } want = KIND_NONE;

    if (dev->proto == HIDKIT_PROTO_MOUSE) {
        want = KIND_MOUSE;
    } else if (dev->proto == HIDKIT_PROTO_KEYBOARD) {
        want = KIND_KB;                       /* boot 协议，固定格式 */
    } else if (dev->proto == HIDKIT_PROTO_GAMEPAD) {
        want = KIND_GAMEPAD;                  /* 适配器（如 XInput）已确认是手柄 */
    } else if (has_desc) {
#if HIDKIT_ENABLE_GAMEPAD
        // 只做 VID:PID 查表（不占槽位）—— 槽位要在下面确认接管后才分配
        if (gamepad_hid_match(dev->vid, dev->pid)) {
            want = KIND_GAMEPAD;              /* 已知手柄布局（VID:PID 命中） */
        } else
#endif
        {
            hid_nkro_desc_t nd;
            memset(&nd, 0, sizeof(nd));
            if (hid_nkro_parse(&nd, dev->report_desc, dev->report_desc_len)) {
                want = KIND_NKRO;             /* NKRO 键盘描述符命中 */
            }
        }
    }

    if (want == KIND_NONE) {
        HIDKIT_LOG("hidkit: unhandled %04x:%04x proto=%u desc=%s\n",
                   dev->vid, dev->pid, (unsigned)dev->proto, has_desc ? "yes" : "none");
        return HIDKIT_UNHANDLED;
    }

    int8_t slot = slot_alloc();
    if (slot < 0) {
        HIDKIT_LOG("hidkit: no slot for %04x:%04x (policy=%d)\n",
                   dev->vid, dev->pid, (int)HIDKIT_OVERFLOW);
        hidkit_input_dropped(-1, dev->vid, dev->pid);
        return slot;
    }

    slot_clear(slot);
    s_slot[slot].kind = (want == KIND_MOUSE) ? SLOT_MOUSE
                      : (want == KIND_KB)    ? SLOT_KEYBOARD
                      : (want == KIND_NKRO)  ? SLOT_NKRO
                                             : SLOT_GAMEPAD;
    s_slot[slot].vid = dev->vid;
    s_slot[slot].pid = dev->pid;
    s_slot[slot].dev_addr = dev->dev_addr;
    s_slot[slot].itf = dev->itf;
    s_slot[slot].proto = dev->proto;
    s_slot[slot].seq = ++s_alloc_seq;

    /* --- 按类型做挂载期解析 --- */
    if (want == KIND_MOUSE) {
        if (has_desc) {
            (void)hid_mouse_parse(&s_mouse[slot].desc, dev->report_desc,
                                  dev->report_desc_len);
        }
        if (s_mouse_count < 255) s_mouse_count++;
    } else if (want == KIND_KB) {
        if (s_kb_count < 255) s_kb_count++;
    } else if (want == KIND_GAMEPAD) {
#if HIDKIT_ENABLE_GAMEPAD
        (void)gamepad_hid_mount(slot, dev->vid, dev->pid, dev->report_desc,
                                dev->report_desc_len);
#endif
    } else if (want == KIND_NKRO) {
        (void)hid_nkro_parse(&s_nkro[slot].desc, dev->report_desc,
                             dev->report_desc_len);
        if (s_kb_count < 255) s_kb_count++;
    }

#if defined(HIDKIT_TICK_MS)
    s_slot[slot].last_tick = (uint32_t)HIDKIT_TICK_MS();
#endif
    HIDKIT_LOG("hidkit: slot %d <- %04x:%04x proto=%u kind=%d\n",
               slot, dev->vid, dev->pid, (unsigned)dev->proto, (int)s_slot[slot].kind);
    return slot;
}

// 每报文的公共入口：四个分支各自的下层 dispatch 都标了 HIDKIT_HOT，
// 入口自己却是普通函数 —— 补上，否则热路径标注在第一次调用处就断了
bool HIDKIT_HOT(hidkit_report)(int8_t slot, const uint8_t *buf, uint16_t len)
{
    if (!hidkit_slot_alive(slot) || !buf || len == 0) return false;
    slot_touch(slot);

    switch (s_slot[slot].kind) {
    case SLOT_MOUSE:
        hid_mouse_dispatch(slot, &s_mouse[slot], buf, len);
        return true;

    case SLOT_KEYBOARD:
        hid_dispatch_keyboard(slot, buf, (uint8_t)len);
        return true;

    case SLOT_NKRO:
        hid_nkro_dispatch(slot, &s_nkro[slot], buf, len);
        return true;

    case SLOT_GAMEPAD: {
#if HIDKIT_ENABLE_GAMEPAD
        hidkit_gamepad_state_t gs;
        if (gamepad_hid_dispatch(slot, buf, len, &gs)) {
            /* 按键边沿 → 统一 key 出口（手柄段前缀 + BTN_*） */
            uint32_t changed = gs.buttons ^ s_slot[slot].last_buttons;
            for (uint8_t i = 0; i < gs.btn_count && i < 32; i++) {
                if (changed & (1u << i)) {
                    uint8_t code = (uint8_t)(gs.btn_map[i] & 0xFF);
                    bool pressed = (gs.buttons >> i) & 1u;
                    hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_GAMEPAD | code), pressed);
                }
            }
            s_slot[slot].last_buttons = gs.buttons;
            s_slot[slot].btn_map = gs.btn_map;
            s_slot[slot].btn_count = gs.btn_count;
            /* 轴：每份解析成功的报文都回调（手柄报文即绝对状态） */
            hidkit_emit_gamepad_abs(slot, gs.ls_x, gs.ls_y, gs.rs_x, gs.rs_y,
                                    gs.lt, gs.rt);
            return true;
        }
#if HIDKIT_ENABLE_ADAPTER_AZERON
        /* Azeron 这类双通道设备：轴走手柄通道（上面的 parse），键盘报文走固定格式。
         * 其键盘报文长度与厂商报文不同 → parse 失败 → 这里按 VID:PID 兜住。 */
        if (s_slot[slot].vid == 0x16D0 && s_slot[slot].pid == 0x13EA && len > 1) {
            hid_dispatch_keyboard(slot, buf + 1, (uint8_t)(len - 1));  /* 跳过 report ID */
            return true;
        }
#endif
        if (!s_slot[slot].logged_unconsumed) {
            s_slot[slot].logged_unconsumed = true;
            HIDKIT_LOG("hidkit: slot %d gamepad report not consumed (len=%u)\n",
                       slot, (unsigned)len);
        }
        return false;   /* 认了设备但这份报文不认识 → 交宿主 */
#else
        return false;
#endif
    }

    default:
        return false;
    }
}

bool hidkit_umount(int8_t slot)
{
    if (!hidkit_slot_alive(slot)) {
        HIDKIT_LOG("hidkit: umount on inactive slot %d\n", (int)slot);
        return false;
    }
    HIDKIT_LOG("hidkit: umount slot %d (%04x:%04x)\n", (int)slot,
               s_slot[slot].vid, s_slot[slot].pid);
    slot_release(slot);
    return true;
}

/*--------------------------------------------------------------------+
 * XInput：适配器把厂商报文归一化成 hidkit_xinput_pad_t 后调用。
 * 出口与 HID 手柄完全同路（同一 key 段前缀 + 同一 gamepad_abs）。
 *--------------------------------------------------------------------*/

/* XInput 标准位 → BTN_*（下标 = XINPUT_GAMEPAD_* 的位序号） */
static const uint16_t k_xinput_btn_map[18] = {
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,          /* 0-3  */
    BTN_START, BTN_SELECT, BTN_LS, BTN_RS,              /* 4-7  */
    BTN_LB, BTN_RB, BTN_HOME, BTN_MISC,                 /* 8-11 */
    BTN_A, BTN_B, BTN_X, BTN_Y,                         /* 12-15 */
    BTN_LT, BTN_RT,                                     /* 16-17：由模拟扳机阈值合成 */
};

/* 模拟扳机 → 数字按键的阈值（与既有实现一致：0..255 中取 102） */
#define HIDKIT_XINPUT_TRIGGER_THRESHOLD 102
/* 扳机 0..255 → 0..32767 */
#define HIDKIT_XINPUT_TRIGGER_SCALE (32767 / 255)

// XInput 路径的每报文入口（适配器 glue 每份报文调一次），与 hidkit_report 对称：
// 一个走 HID 报文、一个走归一化后的 pad，两者都在输入热路径上
bool HIDKIT_HOT(hidkit_xinput_report)(int8_t slot, const hidkit_xinput_pad_t *pad)
{
#if !HIDKIT_ENABLE_GAMEPAD
    (void)slot; (void)pad;
    return false;
#else
    if (!hidkit_slot_alive(slot) || !pad) return false;
    if (s_slot[slot].kind != SLOT_GAMEPAD) return false;
    slot_touch(slot);

    /* 数字位 + 两个由阈值合成的扳机位 */
    uint32_t buttons = pad->buttons;
    if (pad->left_trigger >= HIDKIT_XINPUT_TRIGGER_THRESHOLD)  buttons |= (1u << 16);
    if (pad->right_trigger >= HIDKIT_XINPUT_TRIGGER_THRESHOLD) buttons |= (1u << 17);

    uint32_t changed = buttons ^ s_slot[slot].last_buttons;
    for (uint8_t i = 0; i < 18; i++) {
        if (changed & (1u << i)) {
            uint8_t code = (uint8_t)(k_xinput_btn_map[i] & 0xFF);
            bool pressed = (buttons >> i) & 1u;
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_GAMEPAD | code), pressed);
        }
    }
    s_slot[slot].last_buttons = buttons;

    /* 轴：XInput 的 Y 轴方向与 HID 手柄相反（既有实现的约定），此处保持一致 */
    int32_t const ls_x = pad->lx;
    int32_t const ls_y = -(int32_t)pad->ly;
    int32_t const rs_x = pad->rx;
    int32_t const rs_y = -(int32_t)pad->ry;
    hidkit_emit_gamepad_abs(slot, ls_x, ls_y, rs_x, rs_y,
                            (int32_t)pad->left_trigger  * HIDKIT_XINPUT_TRIGGER_SCALE,
                            (int32_t)pad->right_trigger * HIDKIT_XINPUT_TRIGGER_SCALE);
    return true;
#endif
}

/*--------------------------------------------------------------------+
 * 查询
 *--------------------------------------------------------------------*/

bool hidkit_is_active(int8_t slot) { return hidkit_slot_alive(slot); }

bool hidkit_is_gamepad(int8_t slot)
{
    return hidkit_slot_alive(slot) && s_slot[slot].kind == SLOT_GAMEPAD;
}

uint8_t hidkit_device_type(void)
{
    if (s_mouse_count) return HIDKIT_PROTO_MOUSE;   /* 键鼠同在时报鼠标（与既有语义一致） */
    if (s_kb_count) return HIDKIT_PROTO_KEYBOARD;
    return HIDKIT_PROTO_NONE;
}
