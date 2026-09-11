#include "hid_dispatch.h"

#include "hidkit_config.h"   // HIDKIT_HOT / 容量宏
#include "hidkit_codes.h"    // HIDKIT_CODE_* 段前缀
#include "hidkit_internal.h" // hidkit_emit_*（出口层）

#include <string.h>

#include "hid_struc.h"

//--------------------------------------------------------------------+
// 边沿检测状态（按槽位分槽，仅记上一帧用于变化检测）
//   多键盘/多鼠标共用一份状态会互相污染，见 hid_dispatch.h 的说明。
//--------------------------------------------------------------------+
typedef struct {
    uint8_t last_mouse_btn;
    uint8_t last_keyboard_mods[HIDKIT_MAX_MODS];             // 修饰键字节
    uint8_t last_keyboard_keys[HIDKIT_MAX_KEYS_PER_KB];      // 普通键码
} hid_edge_state_t;

static hid_edge_state_t s_edge[HIDKIT_MAX_SLOTS];

// 取槽位对应的状态；越界返回 NULL（调用方直接丢弃该报文）
static inline hid_edge_state_t *edge_of(int8_t slot)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return NULL;
    return &s_edge[slot];
}

//--------------------------------------------------------------------+
// 鼠标报文解析（mouse_report_x8, 8B 固定格式）
//--------------------------------------------------------------------+

// 与同文件的 hid_dispatch_keyboard 同形同位：boot 协议鼠标的每报文入口。
// 本库内部不调它（走 hid_mouse_dispatch），但宿主可以直接调，标注保持一致
void HIDKIT_HOT(hid_dispatch_mouse)(int8_t slot, const uint8_t *report, uint8_t len)
{
    if (len != sizeof(mouse_report_x8)) return;
    hid_edge_state_t *st = edge_of(slot);
    if (!st) return;

    // 用 memcpy 取报文，避免把（无对齐保证的）报文缓冲强转成结构体指针：
    // Cortex-M0 等平台上的非对齐访问会直接 HardFault。
    mouse_report_x8 m;
    memcpy(&m, report, sizeof(m));

    // 按键变化 → key 事件（鼠标段前缀 + 按键序号）
    uint8_t changed = m.buttons.raw ^ st->last_mouse_btn;
    for (uint8_t bit = 0; bit < HIDKIT_MAX_MOUSE_BUTTONS; bit++) {
        if (changed & (1u << bit)) {
            bool down = (m.buttons.raw >> bit) & 1;
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | bit), down);
        }
    }
    // 位移/滚轮（仅非零时）
    if (m.x != 0 || m.y != 0 || m.wheel != 0) {
        hidkit_emit_mouse_abs(slot, m.x, m.y, m.wheel);
    }
    st->last_mouse_btn = m.buttons.raw;
}

//--------------------------------------------------------------------+
// 键盘报文解析（boot 协议：2 个修饰键字节 + N 个键码槽位）
//--------------------------------------------------------------------+

void HIDKIT_HOT(hid_dispatch_keyboard)(int8_t slot, const uint8_t *report, uint8_t len)
{
    // 至少需要修饰键字节
    if (len < 1) return;

    hid_edge_state_t *st = edge_of(slot);
    if (!st) return;

    uint8_t mod_count = (len < HIDKIT_MAX_MODS) ? len : HIDKIT_MAX_MODS;
    const uint8_t *mods = report;                 // 前几个字节为修饰键
    const uint8_t *keys = report + mod_count;     // 后续为按键数组
    uint8_t key_count = (uint8_t)(len - mod_count);
    if (key_count > HIDKIT_MAX_KEYS_PER_KB) {
        key_count = HIDKIT_MAX_KEYS_PER_KB;       // 截断（上限可配）
    }

    // --- 修饰键处理（每字节 8 位；第 0 字节 → 0xE0+bit，第 1 字节 → 0xE8+bit）---
    for (uint8_t m = 0; m < HIDKIT_MAX_MODS; m++) {
        uint8_t cur = (m < mod_count) ? mods[m] : 0;
        uint8_t changed = (uint8_t)(cur ^ st->last_keyboard_mods[m]);
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (changed & (1u << bit)) {
                bool down = (cur >> bit) & 1;
                uint8_t keycode = (uint8_t)(0xE0 + (m * 8) + bit);
                hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | keycode), down);
            }
        }
        st->last_keyboard_mods[m] = cur;
    }

    // --- 普通键按下检测（当前帧有、上帧没有）---
    for (uint8_t i = 0; i < key_count && keys[i]; i++) {
        bool found = false;
        for (uint8_t j = 0; j < HIDKIT_MAX_KEYS_PER_KB && st->last_keyboard_keys[j]; j++) {
            if (st->last_keyboard_keys[j] == keys[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | keys[i]), true);
        }
    }

    // --- 普通键释放检测（上帧有、当前帧没有）---
    for (uint8_t i = 0; i < HIDKIT_MAX_KEYS_PER_KB && st->last_keyboard_keys[i]; i++) {
        bool found = false;
        for (uint8_t j = 0; j < key_count && keys[j]; j++) {
            if (keys[j] == st->last_keyboard_keys[i]) {
                found = true;
                break;
            }
        }
        if (!found) {
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | st->last_keyboard_keys[i]), false);
        }
    }

    // --- 更新按键状态（清零后复制当前按键，剩余自然为 0）---
    memset(st->last_keyboard_keys, 0, sizeof(st->last_keyboard_keys));
    memcpy(st->last_keyboard_keys, keys, key_count);
}

//--------------------------------------------------------------------+
// 卸载/挤出清理：先补发"全部抬起"，再清空槽位状态
//--------------------------------------------------------------------+

void hid_dispatch_reset(int8_t slot)
{
    hid_edge_state_t *st = edge_of(slot);
    if (!st) return;

    // 修饰键释放
    for (uint8_t m = 0; m < HIDKIT_MAX_MODS; m++) {
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (st->last_keyboard_mods[m] & (1u << bit)) {
                uint8_t keycode = (uint8_t)(0xE0 + (m * 8) + bit);
                hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | keycode), false);
            }
        }
    }

    // 普通键释放
    for (uint8_t i = 0; i < HIDKIT_MAX_KEYS_PER_KB && st->last_keyboard_keys[i]; i++) {
        hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | st->last_keyboard_keys[i]), false);
    }

    // 鼠标按键释放
    for (uint8_t bit = 0; bit < HIDKIT_MAX_MOUSE_BUTTONS; bit++) {
        if (st->last_mouse_btn & (1u << bit)) {
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | bit), false);
        }
    }

    memset(st, 0, sizeof(*st));
}
