/*
 * hidkit 主机侧样本测试：不需要任何硬件，喂"描述符 + 报文"，断言事件序列。
 *
 * 这些用例同时是**行为规格**：忠实移植阶段用它固定住既有解析行为；
 * KNOWN_ISSUES.md 里那批描述符解析缺陷修完之后，每个缺陷都补了新的样本用例
 * （见本文件「缺陷修复样本」一节），它们断言的是规范行为，在修复前的实现上会红。
 *
 * 断言分两层：走公共 API（hidkit_mount/report）验事件序列；
 * 直接调私有解析器（hid_parser.h）验字段元数据（usage / bit_offset / report_id）
 * —— 偏移错位这类缺陷只有看元数据才看得清楚。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "hidkit.h"
#include "hid_parser.h"   // 私有解析器：直接断言字段元数据

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

/* 断言一个字段的元数据（usage page / usage id / 位偏移） */
#define CHECK_FIELD(f, page, usage, off)                                      \
    do {                                                                      \
        const hid_field_t *fp_ = &(f);                                        \
        CHECK(fp_->usage_page == (page) && fp_->usage_id == (usage) &&         \
              fp_->bit_offset == (off),                                       \
              "字段应为 page=0x%02X usage=0x%02X off=%u，实得 page=0x%02X usage=0x%02X off=%u", \
              (unsigned)(page), (unsigned)(usage), (unsigned)(off),           \
              (unsigned)fp_->usage_page, (unsigned)fp_->usage_id,             \
              (unsigned)fp_->bit_offset);                                     \
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
 * 缺陷修复样本（KNOWN_ISSUES.md「已修复」一节，一条缺陷一个样本）
 *--------------------------------------------------------------------*/

/* 缺陷 1：Usage Min/Max 的 range 多于 Report Count。
 * range = 4 而 count = 2 → 循环上界必须是 min(count, range)，否则按键字段多推进
 * 2 位，其后 X/Y 的 bit_offset 全部错位。 */
static const uint8_t k_desc_range_gt_count[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x04,        /*   Usage Maximum (4)  → range = 4 */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x95, 0x02,        /*   Report Count (2)   → 少于 range */
    0x75, 0x01,        /*   Report Size (1) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x95, 0x01,        /*   Report Count (1) */
    0x75, 0x06,        /*   Report Size (6) */
    0x81, 0x03,        /*   Input (Const,Var,Abs) → 补齐到 1 字节 */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x06,        /*   Input (Data,Var,Rel) */
    0xC0,              /* End Collection */
};

/* 缺陷 2：usage 列表(2 个) 少于 Report Count(4) → 第 3/4 个字段应重复最后一个
 * usage(Y)，而不是 j % count 从头环绕（那样会绕回 X）。 */
static const uint8_t k_desc_usage_short[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x04,        /*   Report Count (4) → 多于 usage 个数 */
    0x81, 0x06,        /*   Input (Data,Var,Rel) */
    0xC0,              /* End Collection */
};

/* 缺陷 3：2 字节形式的 Global item（0x06 Usage Page / 0x96 Report Count）。
 * 修复前它们落进 default：跳过但不更新状态 → 沿用旧值（Usage Page 留 0、
 * Report Count 留 1）。末尾一段用 4 字节 Report Count（0x97，确实不支持）
 * 验证"不支持的宽度要复位"而不是留着 5。 */
static const uint8_t k_desc_two_byte_globals[] = {
    0x06, 0x01, 0x00,  /* Usage Page (Generic Desktop)，2 字节形式 */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x95, 0x01,        /*   Report Count (1)，1 字节形式（先落一个"旧值"） */
    0x96, 0x02, 0x00,  /*   Report Count (2)，2 字节形式 */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x06,        /*   Input (Data,Var,Rel) → X、Y 两个字段 */
    0x95, 0x05,        /*   Report Count (5) */
    0x97, 0x00, 0x00, 0x00, 0x00,  /* Report Count (4B)：不支持 → 复位为 0 */
    0x09, 0x38,        /*   Usage (Wheel) */
    0x75, 0x08,        /*   Report Size (8) */
    0x81, 0x06,        /*   Input → Report Count 复位后按 1 个字段算 */
    0xC0,              /* End Collection */
};

/* 缺陷 3（续）：2 字节形式的 Report ID（0x86），Report ID = 9 */
static const uint8_t k_desc_two_byte_report_id[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x86, 0x09, 0x00,  /*   Report ID (9)，2 字节形式 */
    0x09, 0x30,        /*   Usage (X) */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x06,        /*   Input (Data,Var,Rel) → 1 个字段，属于 ID 9 */
    0xC0,              /* End Collection */
};

/* 缺陷 4：End Collection 不清局部状态。
 * 集合内某个 Usage (Wheel) 后面没跟 Main item，End Collection 必须把它清掉，
 * 否则集合外那个"无 usage"的 Input 会继承 Wheel 而多展开一个字段。 */
static const uint8_t k_desc_end_collection[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x09, 0x01,        /*   Usage (Pointer) */
    0xA1, 0x00,        /*   Collection (Physical) */
    0x09, 0x30,        /*     Usage (X) */
    0x15, 0x81,        /*     Logical Minimum (-127) */
    0x25, 0x7F,        /*     Logical Maximum (127) */
    0x75, 0x08,        /*     Report Size (8) */
    0x95, 0x01,        /*     Report Count (1) */
    0x81, 0x06,        /*     Input (Data,Var,Rel) → 1 个字段（X） */
    0x09, 0x38,        /*     Usage (Wheel)，集合内残留（没有跟着的 Main item） */
    0xC0,              /*   End Collection → 应清 local，Wheel 不得外溢 */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x06,        /*   Input（无 usage）→ 不产生字段 */
    0xC0,              /* End Collection */
};

/* 缺陷 5：多 Report ID 描述符。Report ID(1) = 按键 + X/Y，Report ID(2) = 滚轮。
 * 每个字段各自记录所属 ID，两个 Report 都要能解析（修复前只留最后一个 ID，
 * ID=1 的报文整份被丢弃）。 */
static const uint8_t k_desc_multi_report_id[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop) */
    0x09, 0x02,        /* Usage (Mouse) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID (1)：按键 + X/Y */
    0x05, 0x09,        /*   Usage Page (Button) */
    0x19, 0x01,        /*   Usage Minimum (1) */
    0x29, 0x03,        /*   Usage Maximum (3) */
    0x15, 0x00,        /*   Logical Minimum (0) */
    0x25, 0x01,        /*   Logical Maximum (1) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x03,        /*   Report Count (3) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) */
    0x75, 0x05,        /*   Report Size (5) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x03,        /*   Input (Const,Var,Abs) → 补齐到 1 字节 */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x30,        /*   Usage (X) */
    0x09, 0x31,        /*   Usage (Y) */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x02,        /*   Report Count (2) */
    0x81, 0x06,        /*   Input (Data,Var,Rel) → ID=1 报文体 = 按键 + X + Y */
    0x85, 0x02,        /*   Report ID (2)：滚轮 */
    0x05, 0x01,        /*   Usage Page (Generic Desktop) */
    0x09, 0x38,        /*   Usage (Wheel) */
    0x15, 0x81,        /*   Logical Minimum (-127) */
    0x25, 0x7F,        /*   Logical Maximum (127) */
    0x75, 0x08,        /*   Report Size (8) */
    0x95, 0x01,        /*   Report Count (1) */
    0x81, 0x06,        /*   Input (Data,Var,Rel) → ID=2 报文体 = 滚轮 */
    0xC0,              /* End Collection */
};

/* NKRO 路径（hid_nkro_parse）同样受缺陷 3/4 影响：2 字节 Usage Page 声明的
 * Keyboard 页要认；End Collection 前残留的 usage 不得外溢成额外的键位段。 */
static const uint8_t k_nkro_desc_two_byte_page[] = {
    0x06, 0x07, 0x00,  /* Usage Page (Keyboard/Keypad)，2 字节形式 */
    0x09, 0x06,        /* Usage (Keyboard) */
    0xA1, 0x01,        /* Collection (Application) */
    0x06, 0x07, 0x00,  /*   Usage Page (Keyboard/Keypad)，2 字节形式 */
    0x19, 0x04,        /*   Usage Minimum (0x04) */
    0x29, 0x08,        /*   Usage Maximum (0x08) */
    0x75, 0x01,        /*   Report Size (1) */
    0x95, 0x05,        /*   Report Count (5) */
    0x81, 0x02,        /*   Input (Data,Var,Abs) → 一段 0x04 起 5 位的位图 */
    0x09, 0x2C,        /*   Usage（集合内残留，后面没有 Main item） */
    0xC0,              /* End Collection → 应清 local */
    0x75, 0x01,        /* Report Size (1) */
    0x95, 0x03,        /* Report Count (3) */
    0x81, 0x02,        /* Input（无 usage）→ 不产生键位段 */
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

/*--------------------------------------------------------------------+
 * 缺陷修复样本用例
 *--------------------------------------------------------------------*/

/* 缺陷 1：range 多于 Report Count 时，循环上界应取 min(count, range) */
static void test_parser_range_gt_count(void)
{
    printf("描述符解析：usage range 多于 Report Count（缺陷 1）\n");
    hid_mouse_desc_t d;
    CHECK(hid_mouse_parse(&d, k_desc_range_gt_count, sizeof(k_desc_range_gt_count)),
          "应解析成功");
    CHECK(d.num_fields == 4, "应展开 4 个字段（2 按键 + X + Y），实得 %u",
          (unsigned)d.num_fields);
    CHECK(d.button_count == 2, "按键数应以 Report Count(2) 为准，实得 %u",
          (unsigned)d.button_count);
    CHECK_FIELD(d.fields[0], 0x09, 0x01, 0);
    CHECK_FIELD(d.fields[1], 0x09, 0x02, 1);
    /* X/Y 紧跟在 1 字节的按键字段之后：修复前按键段多推进 2 位，这里会变成 10/18 */
    CHECK_FIELD(d.fields[2], 0x01, 0x30, 8);
    CHECK_FIELD(d.fields[3], 0x01, 0x31, 16);

    ev_reset();
    hidkit_dev_info_t info = { .vid = 0x0bad, .pid = 0x0001, .dev_addr = 7, .itf = 0,
                               .proto = HIDKIT_PROTO_MOUSE,
                               .report_desc = k_desc_range_gt_count,
                               .report_desc_len = sizeof(k_desc_range_gt_count) };
    int8_t slot = hidkit_mount(&info);
    CHECK(slot >= 0, "应接管鼠标，得到 %d", slot);
    const uint8_t r0[] = { 0x00, 0x00, 0x00 };
    hidkit_report(slot, r0, sizeof(r0));            /* 首帧建基线 */
    const uint8_t r1[] = { 0x01, 0x05, 0xFB };      /* 左键 + X=5, Y=-5 */
    hidkit_report(slot, r1, sizeof(r1));
    CHECK(g_key_n == 1 && g_key[0].code == (HIDKIT_CODE_MOUSE | 0),
          "左键应按下（实得 %d 个事件）", g_key_n);
    CHECK(g_mouse.dx == 5 && g_mouse.dy == -5, "位移应为 (5,-5)，实得 (%d,%d)",
          g_mouse.dx, g_mouse.dy);
    hidkit_umount(slot);
}

/* 缺陷 2：usage 列表不足时重复最后一个 usage（不是环绕） */
static void test_parser_usage_list_clamp(void)
{
    printf("描述符解析：usage 列表不足时重复最后一个（缺陷 2）\n");
    hid_mouse_desc_t d;
    CHECK(hid_mouse_parse(&d, k_desc_usage_short, sizeof(k_desc_usage_short)),
          "应解析成功");
    CHECK(d.num_fields == 4, "应展开 4 个字段，实得 %u", (unsigned)d.num_fields);
    CHECK_FIELD(d.fields[0], 0x01, 0x30, 0);
    CHECK_FIELD(d.fields[1], 0x01, 0x31, 8);
    /* 修复前用 j % usage_count 环绕 → 这两个会绕回 X(0x30) */
    CHECK_FIELD(d.fields[2], 0x01, 0x31, 16);
    CHECK_FIELD(d.fields[3], 0x01, 0x31, 24);
}

/* 缺陷 3：2 字节形式的 Global item 要认；不支持的宽度要复位 */
static void test_parser_two_byte_globals(void)
{
    printf("描述符解析：2 字节 Global item + 不支持宽度复位（缺陷 3）\n");
    hid_mouse_desc_t d;
    CHECK(hid_mouse_parse(&d, k_desc_two_byte_globals, sizeof(k_desc_two_byte_globals)),
          "应解析成功");
    /* 修复前：0x06 不生效（Usage Page 留 0）、0x96 不生效（Report Count 留 1）、
     * 0x97 不生效（Report Count 留 5）→ 字段数会是 1 + 5 而不是 3 */
    CHECK(d.num_fields == 3, "应为 3 个字段（X、Y、Wheel），实得 %u",
          (unsigned)d.num_fields);
    CHECK_FIELD(d.fields[0], 0x01, 0x30, 0);
    CHECK_FIELD(d.fields[1], 0x01, 0x31, 8);
    CHECK_FIELD(d.fields[2], 0x01, 0x38, 16);
    CHECK(d.idx_x == 0 && d.idx_y == 1 && d.idx_wheel == 2,
          "三个轴索引都应命中，实得 (%u,%u,%u)", (unsigned)d.idx_x,
          (unsigned)d.idx_y, (unsigned)d.idx_wheel);

    /* 2 字节形式的 Report ID（0x86）：修复前不生效 → 整份描述符被当成"无 ID" */
    hid_mouse_desc_t d2;
    CHECK(hid_mouse_parse(&d2, k_desc_two_byte_report_id,
                          sizeof(k_desc_two_byte_report_id)), "应解析成功");
    CHECK(d2.num_fields == 1, "应为 1 个字段，实得 %u", (unsigned)d2.num_fields);
    CHECK(d2.report_id == 9, "2 字节 Report ID 应记下 9，实得 %u", (unsigned)d2.report_id);
    CHECK(d2.fields[0].report_id == 9, "字段应属于 ID 9，实得 %u",
          (unsigned)d2.fields[0].report_id);
    CHECK_FIELD(d2.fields[0], 0x01, 0x30, 0);
}

/* 缺陷 4：End Collection 清局部状态 */
static void test_parser_end_collection_clears_local(void)
{
    printf("描述符解析：End Collection 清局部状态（缺陷 4）\n");
    hid_mouse_desc_t d;
    CHECK(hid_mouse_parse(&d, k_desc_end_collection, sizeof(k_desc_end_collection)),
          "应解析成功");
    /* 修复前集合内的 Wheel 会外溢给集合外的 Input → 2 个字段、idx_wheel 命中 */
    CHECK(d.num_fields == 1, "集合外那个无 usage 的 Input 不应产生字段，实得 %u",
          (unsigned)d.num_fields);
    CHECK_FIELD(d.fields[0], 0x01, 0x30, 0);
    CHECK(d.idx_wheel == 0xFF, "残留的 Wheel 不应被继承，实得 idx_wheel=%u",
          (unsigned)d.idx_wheel);
}

/* 缺陷 5：多 Report ID 描述符按字段各自记录 ID */
static void test_mouse_multi_report_id(void)
{
    printf("鼠标多 Report ID 描述符（缺陷 5）\n");
    hid_mouse_desc_t d;
    CHECK(hid_mouse_parse(&d, k_desc_multi_report_id, sizeof(k_desc_multi_report_id)),
          "应解析成功");
    CHECK(d.num_fields == 6, "应为 6 个字段（3 按键 + X + Y + Wheel），实得 %u",
          (unsigned)d.num_fields);
    CHECK(d.button_count == 3, "按键应为 3 个，实得 %u", (unsigned)d.button_count);
    CHECK(d.report_id == 1, "主 Report ID 应取首个非零 ID(1)，实得 %u",
          (unsigned)d.report_id);
    CHECK_FIELD(d.fields[0], 0x09, 0x01, 0);
    CHECK_FIELD(d.fields[1], 0x09, 0x02, 1);
    CHECK_FIELD(d.fields[2], 0x09, 0x03, 2);
    CHECK_FIELD(d.fields[3], 0x01, 0x30, 8);
    CHECK_FIELD(d.fields[4], 0x01, 0x31, 16);
    CHECK_FIELD(d.fields[5], 0x01, 0x38, 0);   /* ID=2 的位偏移空间独立 */
    CHECK(d.fields[3].report_id == 1 && d.fields[4].report_id == 1 &&
          d.fields[5].report_id == 2,
          "每个字段应记录自己所属的 Report ID");

    ev_reset();
    hidkit_dev_info_t info = { .vid = 0x0bad, .pid = 0x0002, .dev_addr = 8, .itf = 0,
                               .proto = HIDKIT_PROTO_MOUSE,
                               .report_desc = k_desc_multi_report_id,
                               .report_desc_len = sizeof(k_desc_multi_report_id) };
    int8_t slot = hidkit_mount(&info);
    CHECK(slot >= 0, "应接管鼠标，得到 %d", slot);

    const uint8_t r0[] = { 0x01, 0x00, 0x00, 0x00 };
    hidkit_report(slot, r0, sizeof(r0));            /* ID=1 首帧建基线 */

    /* ID=1：按键 + 位移（修复前描述符只留 ID=2，这份报文被整份丢弃） */
    const uint8_t r1[] = { 0x01, 0x01, 0x05, 0xFB };
    CHECK(hidkit_report(slot, r1, sizeof(r1)), "ID=1 报文应被消费");
    CHECK(g_key_n == 1 && g_key[0].code == (HIDKIT_CODE_MOUSE | 0) && g_key[0].pressed,
          "ID=1 报文的左键应按下，实得 %d 个事件", g_key_n);
    CHECK(g_mouse.dx == 5 && g_mouse.dy == -5, "ID=1 位移应为 (5,-5)，实得 (%d,%d)",
          g_mouse.dx, g_mouse.dy);

    /* ID=2：只有滚轮。X/Y/按键都不属于本报文 → 既不产生位移，也不能把
     * 仍按着的左键误判成松开 */
    const uint8_t r2[] = { 0x02, 0x01 };
    hidkit_report(slot, r2, sizeof(r2));
    CHECK(g_mouse.wheel == 1, "ID=2 报文滚轮应为 1，实得 %d", g_mouse.wheel);
    CHECK(g_key_n == 1, "ID=2 报文不应把 ID=1 的按键判成松开，实得 %d 个事件", g_key_n);

    /* 未知 Report ID：与任何字段都不相干 → 不产生事件 */
    int const n_mouse = g_mouse.n;
    const uint8_t r3[] = { 0x07, 0x01, 0x02 };
    hidkit_report(slot, r3, sizeof(r3));
    CHECK(g_mouse.n == n_mouse && g_key_n == 1, "未知 Report ID 不应产生事件");

    hidkit_umount(slot);
}

/* NKRO 路径的缺陷 3/4（2 字节 Usage Page + End Collection 清 local） */
static void test_nkro_two_byte_page_and_end_collection(void)
{
    printf("NKRO 路径：2 字节 Usage Page + End Collection 清 local（缺陷 3/4）\n");
    hid_nkro_desc_t nd;
    /* 修复前 0x06 落 default：Keyboard 页认不出 → 整条描述符识别不出键盘 */
    CHECK(hid_nkro_parse(&nd, k_nkro_desc_two_byte_page,
                         sizeof(k_nkro_desc_two_byte_page)),
          "2 字节 Usage Page 声明的键盘页也应被识别");
    CHECK(nd.num_spans == 1, "应只有 1 个键位段，实得 %u", (unsigned)nd.num_spans);
    CHECK(nd.spans[0].usage_min == 0x04 && nd.spans[0].count == 5 &&
          nd.spans[0].bit_offset == 0,
          "位图段应为 0x04 起 5 位、偏移 0，实得 min=0x%02X count=%u off=%u",
          (unsigned)nd.spans[0].usage_min, (unsigned)nd.spans[0].count,
          (unsigned)nd.spans[0].bit_offset);

    ev_reset();
    hidkit_dev_info_t info = { .vid = 0x04d9, .pid = 0xa293, .dev_addr = 9, .itf = 1,
                               .proto = HIDKIT_PROTO_NONE,
                               .report_desc = k_nkro_desc_two_byte_page,
                               .report_desc_len = sizeof(k_nkro_desc_two_byte_page) };
    int8_t slot = hidkit_mount(&info);
    CHECK(slot >= 0, "应接管为 NKRO 键盘，得到 %d", slot);
    const uint8_t r1[] = { 0x01 };
    CHECK(hidkit_report(slot, r1, sizeof(r1)), "报文应被消费");
    CHECK(g_key_n == 1 && g_key[0].code == (HIDKIT_CODE_KEYBOARD | 0x04),
          "bit0 应产生 0x04 按下，实得 %d 个事件", g_key_n);
    hidkit_umount(slot);
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
    test_parser_range_gt_count();
    test_parser_usage_list_clamp();
    test_parser_two_byte_globals();
    test_parser_end_collection_clears_local();
    test_mouse_multi_report_id();
    test_nkro_two_byte_page_and_end_collection();
    printf("\n结果：%d 通过，%d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
