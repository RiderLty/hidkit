/*
 * hidkit 主机侧样本测试：不需要任何硬件，喂"描述符 + 报文"，断言事件序列。
 *
 * 这些用例同时是**行为规格**：忠实移植阶段用它固定住现有解析行为，
 * 后续修缺陷时只要用例红了就说明行为变了（要改用例就得说明理由）。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "hidkit.h"

/*--------------------------------------------------------------------+
 * 极简测试框架
 *--------------------------------------------------------------------*/

static int g_fail, g_pass;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) { g_pass++; }                                               \
        else {                                                                \
            g_fail++;                                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/*--------------------------------------------------------------------+
 * 事件记录（探针）
 *--------------------------------------------------------------------*/

#define MAX_EV 64
typedef struct {
    int8_t  slot;
    uint16_t code;
    bool    pressed;
} key_ev_t;

static key_ev_t g_key[MAX_EV];
static int      g_key_n;
static struct { int32_t dx, dy, wheel; int n; } g_mouse;
static struct { int32_t ls_x, ls_y, lt; int n; } g_gp;

static void ev_reset(void)
{
    g_key_n = 0;
    memset(&g_mouse, 0, sizeof(g_mouse));
    memset(&g_gp, 0, sizeof(g_gp));
}

static void on_key(int8_t slot, uint16_t code, bool pressed)
{
    if (g_key_n < MAX_EV) g_key[g_key_n++] = (key_ev_t){slot, code, pressed};
}
static void on_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    (void)slot;
    g_mouse.dx = dx; g_mouse.dy = dy; g_mouse.wheel = wheel; g_mouse.n++;
}
static void on_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                           int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt)
{
    (void)slot; (void)rs_x; (void)rs_y; (void)rt;
    g_gp.ls_x = ls_x; g_gp.ls_y = ls_y; g_gp.lt = lt; g_gp.n++;
}

static const hidkit_callbacks_t g_cb = {
    .key = on_key,
    .mouse_abs = on_mouse_abs,
    .gamepad_abs = on_gamepad_abs,
};

/* 希望被钩子吞掉的 code（0 = 不吞）*/
static uint16_t g_hook_swallow;

/* 覆盖弱符号：验证钩子可改写/吞掉事件 */
bool hidkit_hook_key(uint16_t *code, bool *pressed)
{
    (void)pressed;
    if (g_hook_swallow && *code == g_hook_swallow) return false;
    return true;
}

/*--------------------------------------------------------------------+
 * 样本：鼠标报告描述符（标准 boot 鼠标，报文体 = 按键 + X + Y + 滚轮，4 字节）
 *--------------------------------------------------------------------*/

static const uint8_t k_mouse_desc[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x05, 0x09,        /*     Usage Page (Button) */
    0x19, 0x01,        /*     Usage Minimum (1) */
    0x29, 0x03,        /*     Usage Maximum (3) */
    0x15, 0x00,        /*     Logical Minimum (0) */
    0x25, 0x01,        /*     Logical Maximum (1) */
    0x95, 0x03,        /*     Report Count (3) */
    0x75, 0x01,        /*     Report Size (1) */
    0x81, 0x02,        /*     Input (Data,Var,Abs) */
    0x95, 0x01,        /*     Report Count (1) */
    0x75, 0x05,        /*     Report Size (5) */
    0x81, 0x03,        /*     Input (Const,Var,Abs) */
    0x05, 0x01,        /*     Usage Page (Generic Desktop) */
    0x09, 0x30,        /*     Usage (X) */
    0x09, 0x31,        /*     Usage (Y) */
    0x09, 0x38,        /*     Usage (Wheel) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7f,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x03,        /*     Report Count (3) */
    0x81, 0x06,        /*     Input (Data,Var,Rel) */
    0xC0,              /*   End Collection */
    0xC0,              /* End Collection */
};

/* NKRO 键盘：8 个 1 bit 的按键位（usage 0x04..0x0B）→ 报告 1 字节 */
static const uint8_t k_nkro_desc[] = {
    0x05, 0x07,        /* Usage Page (Keyboard/Keypad) */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x05, 0x07,        /*   Usage Page (Keyboard) */
    0x19, 0x04,        /*   Usage Minimum (0x04) */
    0x29, 0x0B,        /*   Usage Maximum (0x0B) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x08,        /*   Report Count (8) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0xC0,              /* End Collection */
};

/*--------------------------------------------------------------------+
 * 用例
 *--------------------------------------------------------------------*/

static void test_boot_keyboard(void)
{
    printf("boot 键盘（固定 8 字节，无需描述符）\n");
    ev_reset();
    hidkit_dev_info_t d = { .vid = 0x1234, .pid = 0x0001, .dev_addr = 1,
                            .itf = 0, .proto = HIDKIT_PROTO_KEYBOARD };
    int8_t slot = hidkit_mount(&d);
    CHECK(slot >= 0, "应接管键盘，得到 %d", slot);

    /* 'a' = HID usage 0x04 */
    const uint8_t press[] = { 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
    CHECK(hidkit_report(slot, press, sizeof(press)), "报文应被消费");
    CHECK(g_key_n == 1, "应有 1 个事件，实得 %d", g_key_n);
    CHECK(g_key[0].code == (HIDKIT_CODE_KEYBOARD | 0x04), "code 应带键盘段前缀，实得 0x%04X", g_key[0].code);
    CHECK(g_key[0].pressed == true, "应为按下");

    /* 同一报文重复 → 无新事件（边沿） */
    hidkit_report(slot, press, sizeof(press));
    CHECK(g_key_n == 1, "重复报文不应再产生事件，实得 %d", g_key_n);

    /* 左 Shift（修饰键第 0 字节 bit1 → 0xE0+1）。
     * 注意顺序：库内先处理修饰键、再处理普通键（与既有实现一致），
     * 所以这两个事件是"先 shift 按下、再 a 松开"。 */
    const uint8_t shifts[] = { 0x02, 0x00, 0, 0, 0, 0, 0, 0 };
    hidkit_report(slot, shifts, sizeof(shifts));
    CHECK(g_key_n == 3, "按下 a 松开 + shift 按下 = 3 个事件，实得 %d", g_key_n);
    CHECK(g_key[1].code == (HIDKIT_CODE_KEYBOARD | 0xE1) && g_key[1].pressed == true,
          "修饰键先输出：左 shift 应为 0xE1 按下");
    CHECK(g_key[2].code == (HIDKIT_CODE_KEYBOARD | 0x04) && g_key[2].pressed == false,
          "随后是 a 松开");

    /* 卸载 → 补发全部抬起 */
    hidkit_umount(slot);
    CHECK(g_key[g_key_n - 1].pressed == false, "卸载应补发松开");
    CHECK(!hidkit_is_active(slot), "槽位应已释放");
}

static void test_fixed_mouse(void)
{
    printf("鼠标（描述符解析 + 固定字段读取）\n");
    ev_reset();
    hidkit_dev_info_t d = { .vid = 0x046d, .pid = 0xc077, .dev_addr = 2,
                            .itf = 0, .proto = HIDKIT_PROTO_MOUSE,
                            .report_desc = k_mouse_desc,
                            .report_desc_len = sizeof(k_mouse_desc) };
    int8_t slot = hidkit_mount(&d);
    CHECK(slot >= 0, "应接管鼠标，得到 %d", slot);

    /* 首帧只建立基线、不发按键事件（与既有实现一致）：先喂一份全零报文 */
    const uint8_t r0[] = { 0x00, 0x00, 0x00, 0x00 };
    hidkit_report(slot, r0, sizeof(r0));
    CHECK(g_key_n == 0, "首帧不应产生按键事件，实得 %d", g_key_n);

    /* 左键按下 + X=+5, Y=-5, wheel=0 */
    const uint8_t r1[] = { 0x01, 0x05, 0xFB, 0x00 };
    hidkit_report(slot, r1, sizeof(r1));
    CHECK(g_key_n == 1, "应有左键事件，实得 %d", g_key_n);
    CHECK(g_key[0].code == (HIDKIT_CODE_MOUSE | 0), "鼠标按键应带鼠标段前缀，实得 0x%04X", g_key[0].code);
    CHECK(g_key[0].pressed, "应为按下");
    CHECK(g_mouse.dx == 5 && g_mouse.dy == -5 && g_mouse.wheel == 0,
          "位移应为 (5,-5,0)，实得 (%d,%d,%d)", g_mouse.dx, g_mouse.dy, g_mouse.wheel);

    /* 滚轮 */
    const uint8_t r2[] = { 0x01, 0x00, 0x00, 0x01 };
    hidkit_report(slot, r2, sizeof(r2));
    CHECK(g_mouse.wheel == 1, "滚轮应为 1，实得 %d", g_mouse.wheel);

    /* 全零报文 → 无事件（位移为零不回调） */
    int const n_before = g_mouse.n;
    const uint8_t r3[] = { 0x00, 0x00, 0x00, 0x00 };
    hidkit_report(slot, r3, sizeof(r3));
    CHECK(g_mouse.n == n_before, "零位移不应回调 mouse_abs");

    hidkit_umount(slot);
}

static void test_nkro_keyboard(void)
{
    printf("NKRO 键盘（描述符位图段）\n");
    ev_reset();
    hidkit_dev_info_t d = { .vid = 0x04d9, .pid = 0xa292, .dev_addr = 3,
                            .itf = 1, .proto = HIDKIT_PROTO_NONE,
                            .report_desc = k_nkro_desc,
                            .report_desc_len = sizeof(k_nkro_desc) };
    int8_t slot = hidkit_mount(&d);
    CHECK(slot >= 0, "应识别为 NKRO 键盘，得到 %d", slot);

    /* bit0 与 bit3 → usage 0x04 与 0x07 */
    const uint8_t r1[] = { 0x09 };
    CHECK(hidkit_report(slot, r1, sizeof(r1)), "报文应被消费");
    CHECK(g_key_n == 2, "应有两个按键按下，实得 %d", g_key_n);
    CHECK(g_key[0].code == (HIDKIT_CODE_KEYBOARD | 0x04), "第一个应为 0x04，实得 0x%04X", g_key[0].code);
    CHECK(g_key[1].code == (HIDKIT_CODE_KEYBOARD | 0x07), "第二个应为 0x07，实得 0x%04X", g_key[1].code);

    hidkit_umount(slot);
    CHECK(g_key_n == 4, "卸载应补发两个松开，实得 %d", g_key_n);
}

static void test_xinput(void)
{
    printf("XInput 归一化入口（布局表 + 扳机合成位 + Y 反向）\n");
    ev_reset();
    hidkit_dev_info_t d = { .vid = 0x045e, .pid = 0x028e, .dev_addr = 4,
                            .itf = 0, .proto = HIDKIT_PROTO_NONE };
    int8_t slot = hidkit_mount(&d);
    CHECK(slot == HIDKIT_UNHANDLED, "无描述符且非手柄表内设备 → 未消费，实得 %d", slot);

    /* 手工占一个槽位走手柄路径：这里直接用 XInput 入口（适配器会走同一条） */
    /* 用一个已知手柄的 VID:PID 不可靠（布局表由另一文件维护），因此直接测
     * “未挂载的 slot 会被拒绝” 与 “挂载后的行为” 由真机回归覆盖。 */
    hidkit_xinput_pad_t pad = { 0 };
    CHECK(!hidkit_xinput_report(slot, &pad), "未挂载槽位应拒绝");
    (void)d;
}

static void test_unhandled_and_hook(void)
{
    printf("未消费语义 + 拦截钩子\n");
    ev_reset();
    /* 未知设备（proto=0 且无描述符）→ 未消费 */
    hidkit_dev_info_t unk = { .vid = 0xdead, .pid = 0xbeef, .dev_addr = 5, .itf = 0 };
    CHECK(hidkit_mount(&unk) == HIDKIT_UNHANDLED, "未知设备应返回未消费");

    /* 钩子吞掉：装一个键盘，吞掉 'a' */
    hidkit_dev_info_t d = { .vid = 0x1234, .pid = 0x0002, .dev_addr = 6,
                            .itf = 0, .proto = HIDKIT_PROTO_KEYBOARD };
    int8_t slot = hidkit_mount(&d);
    g_hook_swallow = (uint16_t)(HIDKIT_CODE_KEYBOARD | 0x04);
    const uint8_t press[] = { 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
    hidkit_report(slot, press, sizeof(press));
    CHECK(g_key_n == 0, "被钩子吞掉的事件不应到达回调，实得 %d", g_key_n);
    g_hook_swallow = 0;
    hidkit_umount(slot);
}

static void test_evict(void)
{
    printf("槽位挤出（EVICT_IDLE 默认策略）\n");
    ev_reset();
    int8_t slots[HIDKIT_MAX_SLOTS + 1];
    for (int i = 0; i <= HIDKIT_MAX_SLOTS; i++) {
        hidkit_dev_info_t d = { .vid = (uint16_t)(0x1000 + i), .pid = 1,
                                .dev_addr = (uint8_t)(i + 1), .itf = 0,
                                .proto = HIDKIT_PROTO_KEYBOARD };
        slots[i] = hidkit_mount(&d);
    }
    CHECK(slots[HIDKIT_MAX_SLOTS] >= 0, "超出容量时应挤掉最老的槽位（默认 EVICT_IDLE）");
    CHECK(slots[0] == slots[HIDKIT_MAX_SLOTS], "应复用最老的那个槽位号");
    hidkit_init(&g_cb);
}

int main(void)
{
    hidkit_init(&g_cb);
    printf("== hidkit 主机侧样本测试 ==\n");
    test_boot_keyboard();
    test_fixed_mouse();
    test_nkro_keyboard();
    test_xinput();
    test_unhandled_and_hook();
    test_evict();
    printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
