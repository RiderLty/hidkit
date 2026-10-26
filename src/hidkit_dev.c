/*
 * hidkit 出口层：槽位管理 + 入口分发 + 事件出口。
 *
 * 这一层是唯一"知道设备类型怎么判定"的地方。判定原则是**描述符优先**：
 *   - 有报告描述符：解析出能力集（NKRO 键盘段 / 鼠标字段 / VID:PID 手柄布局），
 *     同一个接口可以同时带多种集合（多 Report ID 复合描述符），槽位保存能力位，
 *     报文按 Report ID 路由到对应解析器。
 *   - 没有描述符（超过枚举缓冲 / 固定格式设备）：只能用 proto 兜底：
 *     KEYBOARD → boot 固定格式、MOUSE → 固定 8 字节、GAMEPAD → 手柄布局表。
 *
 * 为什么不能只看 bInterfaceProtocol：HID 规范里协议字节只在 boot 子类下有意义，
 * 且不少复合设备（如 2.4G 接收器）会把带 NKRO 键盘集合的接口声明成 Mouse。
 * 旧实现因此把整份键盘集合丢给鼠标解析器，键事件被静默消费。
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

/* 槽位能力位：kind 只是"主类型"（供查询/日志），实际能吃什么报文由 caps 决定。
 * 一个接口可能同时具备多种能力（多 Report ID 复合描述符）。 */
enum {
    CAP_MOUSE_DESC = 1u << 0,  /* 描述符鼠标   → hid_mouse_dispatch */
    CAP_MOUSE_BOOT = 1u << 1,  /* 无描述符固定 8 字节鼠标 → hid_dispatch_mouse */
    CAP_KB_NKRO    = 1u << 2,  /* 描述符键盘位段 → hid_nkro_dispatch（boot 描述符也能走） */
    CAP_KB_BOOT    = 1u << 3,  /* 无描述符 boot 键盘 → hid_dispatch_keyboard */
    CAP_GAMEPAD    = 1u << 4,  /* VID:PID 手柄布局 / 适配器通道 */
};

#define CAP_ANY_KB (CAP_KB_NKRO | CAP_KB_BOOT)
#define CAP_ANY_MOUSE (CAP_MOUSE_DESC | CAP_MOUSE_BOOT)

typedef struct {
    slot_kind_t kind;
    uint8_t  caps;                         /* CAP_* 位 */
    uint16_t vid, pid;
    uint8_t  dev_addr, itf, proto;
    uint32_t last_buttons;                 /* 手柄按键边沿用 */
    const uint16_t *btn_map;               /* 手柄布局表（指向静态表，卸载时补发松开要用） */
    uint8_t  btn_count;
    bool     has_motion;                   /* 布局声明含六轴（卸载时补发全零要用） */
    bool     has_touchpad;                 /* 布局声明含触摸板（卸载时补发全零要用） */
    uint32_t seq;                          /* 分配序号：EVICT_IDLE 的退化判据 */
    bool     logged_unconsumed;             /* 该槽位的「报文未消费」只报一次 */
    uint16_t boot_kb_bits;                  /* CAP_KB_BOOT 且描述符可读时，描述符声明的
                                             * 该 Report 总位宽（0 = 不校验） */
    uint8_t  boot_kb_id;                    /* CAP_KB_BOOT 时该 Report 的 ID（0 = 无前缀） */
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
                             int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt,
                             uint32_t buttons)
{
    if (!hidkit_hook_gamepad_abs(&ls_x, &ls_y, &rs_x, &rs_y, &lt, &rt, &buttons)) return;
    hidkit_input_gamepad_abs(slot, ls_x, ls_y, rs_x, rs_y, lt, rt, buttons);
}

void hidkit_emit_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                int16_t ax, int16_t ay, int16_t az)
{
    if (!hidkit_hook_gamepad_motion(&gx, &gy, &gz, &ax, &ay, &az)) return;
    hidkit_input_gamepad_motion(slot, gx, gy, gz, ax, ay, az);
}

void hidkit_emit_touchpad(int8_t slot, const hidkit_touchpad_t *tp)
{
    hidkit_touchpad_t snap = *tp;
    if (!hidkit_hook_touchpad(&snap)) return;
    hidkit_input_touchpad(slot, &snap);
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

/* 释放一个槽位：先补发"全部抬起"，再清状态（卸载与挤出共用） */
static void slot_release(int8_t slot)
{
    uint8_t const caps = s_slot[slot].caps;

    if (caps & CAP_KB_NKRO) {
        hid_nkro_release_all(slot, &s_nkro[slot]);
    }
    if (caps & CAP_MOUSE_DESC) {
        /* 描述符鼠标的按键状态在 s_mouse（不是 hid_dispatch 那份），单独补发 */
        hid_mouse_release_all(slot, &s_mouse[slot]);
    }
    hid_dispatch_reset(slot);            /* boot 键鼠的补发松开（固定鼠标按键也在这里） */

    if (caps & CAP_GAMEPAD) {
#if HIDKIT_ENABLE_GAMEPAD
        /* 手柄按键补发松开（按布局表查回 BTN_* 再走统一 key 出口），轴归零 */
        const uint16_t *map = s_slot[slot].btn_map;
        uint8_t count = s_slot[slot].btn_count;
        for (uint8_t i = 0; map && i < count && i < 32; i++) {
            if (s_slot[slot].last_buttons & (1u << i)) {
                uint8_t code = (uint8_t)(map[i] & 0xFF);
                if (!code) continue; // 未映射的保留位不能生成 0x0200 松开事件
                hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_GAMEPAD | code), false);
            }
        }
        hidkit_emit_gamepad_abs(slot, 0, 0, 0, 0, 0, 0, 0);
        /* 六轴补发全零：消费方没有 umount 通知，补零就是"设备已离开"的信号
         * （角速度若停在旧值上，做积分的解算器会永久漂移 —— 与补发"全部
         * 松开"同一语义）。补零走完整链路（hook → input），钩子可在断连帧
         * 做自己的清理；accel 全零语义是"失重"而非"静止"，只此一帧。 */
        if (s_slot[slot].has_motion)
            hidkit_emit_gamepad_motion(slot, 0, 0, 0, 0, 0, 0);
        /* 触摸板补发全零快照（"设备已离开"信号，与六轴补零同一语义；
         * 消费方去重保证只在之前有触点时真正送达一次） */
        if (s_slot[slot].has_touchpad) {
            hidkit_touchpad_t zero = {0};
            hidkit_emit_touchpad(slot, &zero);
        }
        /* 清掉手柄模块自己的状态（parse 指针 / active / Switch 校准）。
         * 不调它的话：卸载后 gamepad_is_active() 仍为真，槽位被复用成键盘后
         * valid_gp_pid_vid() 仍按旧手柄匹配，hidkit_switchpro_set_calib() 对
         * 已释放的槽位继续返回 true —— 与 hidkit.h 的"卸载后自动失效"契约相反。 */
        gamepad_hid_umount(slot);
#endif
    }

    if ((caps & CAP_ANY_KB) && s_kb_count) s_kb_count--;
    if ((caps & CAP_ANY_MOUSE) && s_mouse_count) s_mouse_count--;

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
    /* 先复位边沿检测状态，再清槽位。
     * 只清槽位是不够的：hid_dispatch.c 的 s_edge[] 与 gamepad.c 的 g_gp[]
     * 是各自模块的静态状态，slot_clear() 不碰它们。按着键调 hidkit_init()、
     * 再重挂同一设备、再按同一个键 → 旧实现零事件（边沿被判成"未变化"）。 */
    for (int8_t i = 0; i < (int8_t)HIDKIT_MAX_SLOTS; i++) {
        hid_dispatch_reset(i);
        gamepad_hid_umount(i);
        slot_clear(i);
    }
    s_alloc_seq = 0;
    s_kb_count = 0;
    s_mouse_count = 0;
}

/* 鼠标描述符是否真的解析出了可用字段（避免只凭"有 Mouse 集合"就空认领） */
static bool mouse_desc_usable(const hid_mouse_desc_t *d)
{
    return d->idx_x != 0xFF || d->idx_y != 0xFF || d->idx_wheel != 0xFF ||
           d->button_count > 0;
}

int8_t hidkit_mount(const hidkit_dev_info_t *dev)
{
    if (!dev) return HIDKIT_UNHANDLED;
    const bool has_desc = (dev->report_desc && dev->report_desc_len > 0);

    /* --- 先判定能力集（不认识就不占槽位）--- */
    uint8_t caps = 0;
    uint16_t boot_kb_bits = 0;               /* 描述符声明的 boot 键盘报文位宽 */
    uint8_t  boot_kb_id   = 0;               /* 该报文是否有 Report ID 前缀 */

#if HIDKIT_ENABLE_GAMEPAD
    if (dev->proto == HIDKIT_PROTO_GAMEPAD) {
        caps = CAP_GAMEPAD;                   /* 适配器（如 XInput）已确认是手柄 */
    }
#else
    /* 手柄支持被裁掉：不能认领 proto=GAMEPAD —— 认了却没有任何解析器接手，
     * 槽位被永久占住且 hidkit_report() 恒返回 false（比交宿主更糟）。 */
    if (dev->proto == HIDKIT_PROTO_GAMEPAD) {
        HIDKIT_LOG("hidkit: gamepad support disabled, skip %04x:%04x\n",
                   dev->vid, dev->pid);
        return HIDKIT_UNHANDLED;
    }
#endif
#if HIDKIT_ENABLE_GAMEPAD
    else if (dev->proto == HIDKIT_PROTO_NONE && has_desc &&
             gamepad_hid_match(dev->vid, dev->pid)) {
        /* 已知手柄布局（VID:PID 命中）。必须排在描述符解析之前：手柄描述符里
         * 同样有 Generic Desktop X/Y/Button，先解析描述符会被误判成鼠标。 */
        caps = CAP_GAMEPAD;
    }
#endif
    else if (has_desc) {
        /* 描述符优先 —— bInterfaceProtocol 只作参考（复合设备常谎报 proto） */
        hid_nkro_desc_t nd;
        memset(&nd, 0, sizeof(nd));
        if (hid_nkro_parse(&nd, dev->report_desc, dev->report_desc_len)) {
            caps |= CAP_KB_NKRO;              /* 描述符里有键盘键位段 */
        }

        /* 鼠标能力的认领：描述符里有 Mouse/Pointer 顶层集合（键盘+鼠标复合
         * 接口常只写 proto=Keyboard/0），或 proto 明确是 Mouse（描述符可能
         * 只截到了不完整的一部分）。两条路都要求确实解析出鼠标字段 ——
         * 认了却解析不出字段，报文会被静默消费掉（既不出事件、也不交宿主），
         * 比不认领更糟。 */
        bool mouse_like = hid_desc_has_mouse_collection(dev->report_desc,
                                                        dev->report_desc_len) ||
                          dev->proto == HIDKIT_PROTO_MOUSE;
        if (mouse_like) {
            hid_mouse_desc_t md;
            memset(&md, 0, sizeof(md));
            if (hid_mouse_parse(&md, dev->report_desc, dev->report_desc_len) &&
                mouse_desc_usable(&md)) {
                caps |= CAP_MOUSE_DESC;
            } else if (dev->proto == HIDKIT_PROTO_MOUSE) {
                /* 描述符没读懂却自称鼠标（厂商私有格式 / 描述符截断）：
                 * 回落到固定 8 字节格式，而不是空认领。 */
                caps |= CAP_MOUSE_BOOT;
            }
        }

        if (caps == 0) {
            /* 描述符里没有本库认识的集合（或宿主只抓到了一部分）→ 回落到 proto。
             * MOUSE 已在上面认领，走到这里的只可能是 KEYBOARD（描述符解析失败）。
             * 回落是"描述符没读懂"的下策，必须先确认描述符**确实声明了键盘集合**：
             * 描述符通篇没有键盘 collection 的接口（Consumer/厂商集合）不是键盘，
             * 谎报的 proto 不能让我们把它的报文解码成按键。
             * 同时记下描述符声明的键盘报文位宽，报文长度不符时同样不解。 */
            if (dev->proto == HIDKIT_PROTO_KEYBOARD) {
                hid_report_len_t lb[HID_REPORT_LEN_TABLE];
                uint8_t const n = hid_desc_report_bits(dev->report_desc,
                                                       dev->report_desc_len,
                                                       lb, HID_REPORT_LEN_TABLE);
                for (uint8_t i = 0; i < n; i++) {
                    if (lb[i].bits_kb) {
                        caps = CAP_KB_BOOT;
                        boot_kb_bits = lb[i].bits_kb;  /* 键盘页 Input 的位宽 */
                        boot_kb_id   = lb[i].id;
                        break;
                    }
                }
                HIDKIT_LOG("hidkit: boot fallback %04x:%04x -> %s (kb bits=%u)\n",
                           dev->vid, dev->pid, caps ? "kb" : "none",
                           (unsigned)boot_kb_bits);
            }
        }
    } else {
        /* 无描述符（超过枚举缓冲 / 固定格式设备）：proto 是唯一线索 */
        if (dev->proto == HIDKIT_PROTO_KEYBOARD)      caps = CAP_KB_BOOT;
        else if (dev->proto == HIDKIT_PROTO_MOUSE)    caps = CAP_MOUSE_BOOT;
    }

    if (caps == 0) {
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
    s_slot[slot].caps = caps;
    s_slot[slot].kind = (caps & CAP_GAMEPAD)  ? SLOT_GAMEPAD
                      : (caps & CAP_KB_NKRO)  ? SLOT_NKRO
                      : (caps & CAP_KB_BOOT)  ? SLOT_KEYBOARD
                                              : SLOT_MOUSE;
    s_slot[slot].vid = dev->vid;
    s_slot[slot].pid = dev->pid;
    s_slot[slot].dev_addr = dev->dev_addr;
    s_slot[slot].itf = dev->itf;
    s_slot[slot].proto = dev->proto;
    s_slot[slot].seq = ++s_alloc_seq;
    s_slot[slot].boot_kb_bits = boot_kb_bits;
    s_slot[slot].boot_kb_id = boot_kb_id;

    /* --- 按能力做挂载期解析 --- */
    if (caps & CAP_GAMEPAD) {
#if HIDKIT_ENABLE_GAMEPAD
        (void)gamepad_hid_mount(slot, dev->vid, dev->pid, dev->report_desc,
                                dev->report_desc_len);
#endif
    }
    if (caps & CAP_KB_NKRO) {
        (void)hid_nkro_parse(&s_nkro[slot].desc, dev->report_desc,
                             dev->report_desc_len);
    }
    if (caps & CAP_MOUSE_DESC) {
        (void)hid_mouse_parse(&s_mouse[slot].desc, dev->report_desc,
                              dev->report_desc_len);
    }
    if ((caps & CAP_ANY_KB) && s_kb_count < 255) s_kb_count++;
    if ((caps & CAP_ANY_MOUSE) && s_mouse_count < 255) s_mouse_count++;

#if defined(HIDKIT_TICK_MS)
    s_slot[slot].last_tick = (uint32_t)HIDKIT_TICK_MS();
#endif
    HIDKIT_LOG("hidkit: slot %d <- %04x:%04x proto=%u kind=%d caps=0x%02X\n",
               slot, dev->vid, dev->pid, (unsigned)dev->proto,
               (int)s_slot[slot].kind, (unsigned)caps);
    return slot;
}

// 每报文的公共入口：四个分支各自的下层 dispatch 都标了 HIDKIT_HOT，
// 入口自己却是普通函数 —— 补上，否则热路径标注在第一次调用处就断了
bool HIDKIT_HOT(hidkit_report)(int8_t slot, const uint8_t *buf, uint16_t len)
{
    if (!hidkit_slot_alive(slot) || !buf || len == 0) return false;
    slot_touch(slot);

    uint8_t const caps = s_slot[slot].caps;

    /* 一个接口可能带多种集合（多 Report ID 复合描述符）：按报文首字节
     * （Report ID）路由，再用描述符声明的报文长度复核 —— 长度不够的报文
     * 连自己的字段都装不下，绝不能拿去解（真机 BE EF → dx=-4162）。 */
    uint8_t const kb_score = (caps & CAP_KB_NKRO)
                           ? hid_nkro_match(&s_nkro[slot], buf, len) : 0;
    uint8_t const ms_score = (caps & CAP_MOUSE_DESC)
                           ? hid_mouse_match(&s_mouse[slot], buf, len) : 0;
    bool const kb_ok = kb_score > 0;
    bool const ms_ok = ms_score > 0;

    /* 键鼠共用一份报文（同一个显式 Report ID，或描述符根本不用 Report ID）：
     * 两份解析器各自按自己的位偏移处理本报文 —— 对齐 Linux hid-input 的
     * 逐字段模型，不能"键盘优先"把鼠标那半边吃掉。
     *
     * 能走到这里的先决条件已经是**长度校验通过**：不用 Report ID 的描述符
     * 只有一种报文长度（所有集合合在一份报文里），键盘与鼠标看到的是同一条
     * 长度判据，所以两者同时命中正说明这份报文确实同时携带两类字段。 */
    if (kb_ok && ms_ok && kb_score == ms_score) {
        hid_nkro_dispatch(slot, &s_nkro[slot], buf, len);
        hid_mouse_dispatch(slot, &s_mouse[slot], buf, len);
        return true;
    }
    if (kb_ok && ms_ok) {
        /* 长度只有一方精确吻合（同一接口上键鼠各用一段位宽、整体长度可兼容）：
         * 精确吻合的一方胜出。 */
        if (ms_score > kb_score) {
            hid_mouse_dispatch(slot, &s_mouse[slot], buf, len);
        } else {
            hid_nkro_dispatch(slot, &s_nkro[slot], buf, len);
        }
        return true;
    }
    if (kb_ok) {
        hid_nkro_dispatch(slot, &s_nkro[slot], buf, len);
        return true;
    }
    if (ms_ok) {
        hid_mouse_dispatch(slot, &s_mouse[slot], buf, len);
        return true;
    }
    if (caps & CAP_KB_BOOT) {
        bool decode = true;
        const uint8_t *body = buf;
        uint16_t body_len = len;
        uint16_t const kb_bits = s_slot[slot].boot_kb_bits;
        uint8_t const kb_id = s_slot[slot].boot_kb_id;

        if (kb_id) {
            /* 描述符里键盘集合带 Report ID：报文首字节必须是它 */
            decode = (len >= 1 && buf[0] == kb_id);
            if (decode) {
                body = buf + 1;
                body_len = (uint16_t)(len - 1);
            }
        }
        if (decode && kb_bits) {
            /* 描述符可读：boot 解码器只认 2 个修饰键字节 + 键码数组的布局，
             * 报文必须装得下描述符声明的键盘数据体。短报文（同一接口上的
             * 鼠标/厂商报文）交给宿主，不能解码成按键。 */
            uint16_t const want = (uint16_t)((kb_bits + 7u) / 8u);
            decode = (want != 0 && body_len >= want);
        } else if (decode) {
            /* 描述符不可读：只能按 boot 键盘报文的合法长度校验（HID 规范
             * 6~8 字节，含 2 修饰键字节的变体）。更短的报文不可能是键盘报文。 */
            decode = (body_len >= 6 && body_len <= 8);
        }
        /* boot 解码器的 len 参数是 uint8_t：>= 256 的报文会被截断成低 8 位
         * （264 → 8），随后被当成一份合法的 boot 报文解出假位移/假按键。
         * 长度校验用的是未截断的 body_len，所以这里必须先挡住。 */
        if (decode && body_len <= 255) {
            hid_dispatch_keyboard(slot, body, (uint8_t)body_len);
            return true;
        }
    }
    if (caps & CAP_MOUSE_BOOT) {
        /* 同上：固定 8 字节格式只认 len == 8，截断会让 264 字节报文变成 8 */
        if (len <= 255) {
            hid_dispatch_mouse(slot, buf, (uint8_t)len);
            return true;
        }
    }

    if (caps & CAP_GAMEPAD) {
#if HIDKIT_ENABLE_GAMEPAD
        /* 控制应答只表示命令完成，不能读取状态或覆盖按键/运动基线。
         * Switch 的 0x21/0x81 由宿主握手模块处理，仍向宿主返回已消费。 */
        if (gamepad_hid_is_reply(slot, buf, len)) return true;
        hidkit_gamepad_state_t gs = {0};
        if (gamepad_hid_dispatch(slot, buf, len, &gs)) {
            /* 按键边沿 → 统一 key 出口（手柄段前缀 + BTN_*） */
            uint32_t changed = gs.buttons ^ s_slot[slot].last_buttons;
            for (uint8_t i = 0; i < gs.btn_count && i < 32; i++) {
                if (changed & (1u << i)) {
                    uint8_t code = (uint8_t)(gs.btn_map[i] & 0xFF);
                    if (!code) continue; // BTN 映射为 0 表示忽略该位
                    bool pressed = (gs.buttons >> i) & 1u;
                    hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_GAMEPAD | code), pressed);
                }
            }
            s_slot[slot].last_buttons = gs.buttons;
            s_slot[slot].btn_map = gs.btn_map;
            s_slot[slot].btn_count = gs.btn_count;
            s_slot[slot].has_motion = gs.has_motion;
            s_slot[slot].has_touchpad = gs.has_touchpad;
            /* 轴 + 原生按钮位图：每份解析成功的报文都回调（手柄报文即绝对状态） */
            hidkit_emit_gamepad_abs(slot, gs.ls_x, gs.ls_y, gs.rs_x, gs.rs_y,
                                    gs.lt, gs.rt, gs.buttons);
            /* 六轴：仅布局声明 has_motion 的报文回调（无 IMU 的手柄不发，
             * 0 对陀螺仪是合法读数，不能伪造） */
            if (gs.has_motion)
                hidkit_emit_gamepad_motion(slot, gs.gyro_x, gs.gyro_y, gs.gyro_z,
                                           gs.accel_x, gs.accel_y, gs.accel_z);
            /* 触摸板：仅布局声明触摸板的报文回调（每报文必发，去重是消费方策略） */
            if (gs.has_touchpad)
                hidkit_emit_touchpad(slot, &gs.touchpad);
            return true;
        }
#if HIDKIT_ENABLE_ADAPTER_AZERON
        /* Azeron 这类双通道设备：轴走手柄通道（上面的 parse），键盘报文走固定格式。
         * 其键盘报文长度与厂商报文不同 → parse 失败 → 这里按 VID:PID 兜住。 */
        if (s_slot[slot].vid == 0x16D0 && s_slot[slot].pid == 0x13EA &&
            len > 1 && len - 1 <= 255) {
            hid_dispatch_keyboard(slot, buf + 1, (uint8_t)(len - 1));  /* 跳过 report ID */
            return true;
        }
#endif
        if (!s_slot[slot].logged_unconsumed) {
            s_slot[slot].logged_unconsumed = true;
            HIDKIT_LOG("hidkit: slot %d gamepad report not consumed (len=%u, id=0x%02X)\n",
                       slot, (unsigned)len, (unsigned)buf[0]);
        }
        return false;   /* 认了设备但这份报文不认识 → 交宿主 */
#else
        return false;
#endif
    }

    /* 认了设备，但这份报文（Report ID）不属于任何已解析集合（如厂商/Consumer
     * 报表）→ 报"未消费"让宿主兜底。每个槽位只自证一次，避免逐帧刷屏。 */
    if (!s_slot[slot].logged_unconsumed) {
        s_slot[slot].logged_unconsumed = true;
        HIDKIT_LOG("hidkit: slot %d report not consumed (len=%u, id=0x%02X)\n",
                   slot, (unsigned)len, (unsigned)buf[0]);
    }
    return false;
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

/* XInput 标准位 → BTN_*（下标 = XINPUT_GAMEPAD_* 的位序号）。
 * 只在手柄功能打开时用到 —— 关掉 HIDKIT_ENABLE_GAMEPAD 后整段入口走空返回，
 * 这张表不再被引用，留着会触发 -Wunused-const-variable（-Werror 下编不过）。 */
#if HIDKIT_ENABLE_GAMEPAD
static const uint16_t k_xinput_btn_map[18] = {
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT,          /* 0-3  */
    BTN_START, BTN_SELECT, BTN_LS, BTN_RS,              /* 4-7  */
    BTN_LB, BTN_RB, BTN_HOME, BTN_MISC,                 /* 8-11 */
    BTN_A, BTN_B, BTN_X, BTN_Y,                         /* 12-15 */
    BTN_LT, BTN_RT,                                     /* 16-17：由模拟扳机阈值合成 */
};
#endif  /* HIDKIT_ENABLE_GAMEPAD */

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

    /* 记下布局表：卸载/挤出时 slot_release() 靠 btn_map 把按住的键补发松开。
     * 不填的话 map 为 NULL，松开循环整个跳过 —— 实测按住 A+Start 卸载后
     * 0 个松开事件，上层映射引擎里这些键永久卡在按下。 */
    s_slot[slot].btn_map = k_xinput_btn_map;
    s_slot[slot].btn_count = 18;

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
                            (int32_t)pad->right_trigger * HIDKIT_XINPUT_TRIGGER_SCALE,
                            buttons);
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
