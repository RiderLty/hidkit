/**
 * @file hid_parser.c
 * @brief HID 报告描述符解析 + dispatch 实现
 *
 * 参考 HID 1.11 Device Class Definition §6.2.2 描述符格式。
 */

#include "hid_parser.h"
#include "hidkit_codes.h"    // HIDKIT_CODE_* 段前缀
#include "hidkit_internal.h" // hidkit_emit_*（出口层）
#include <string.h>          // memset

/*--------------------------------------------------------------------+
 * 辅助宏
 *--------------------------------------------------------------------*/

// 短 item 的数据长度（字节）
// bSize: 0=0B, 1=1B, 2=2B, 3=4B
#define ITEM_DATA_SIZE(prefix) \
    (((prefix) & 0x03) < 3 ? ((prefix) & 0x03) : 4)

/*--------------------------------------------------------------------+
 * 辅助函数
 *--------------------------------------------------------------------*/

// 从小端数据中读取有符号整数
static int32_t read_signed(const uint8_t *data, uint8_t size)
{
    uint32_t raw = 0;
    for (uint8_t i = 0; i < size && i < 4; i++)
        raw |= (uint32_t)data[i] << (8 * i);
    switch (size) {
    case 1: return (int32_t)(int8_t)raw;
    case 2: return (int32_t)(int16_t)raw;
    default: return (int32_t)raw;
    }
}

// 从小端数据中读取无符号整数
static uint32_t read_unsigned(const uint8_t *data, uint8_t size)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < size && i < 4; i++)
        value |= (uint32_t)data[i] << (8 * i);
    return value;
}

/*--------------------------------------------------------------------+
 * 通用描述符字段解析（共享 — mouse/gamepad 共用）
 *--------------------------------------------------------------------*/

uint8_t hid_parse_report_fields(hid_field_t *fields, uint8_t max_fields,
                                 uint8_t *report_id,
                                 const uint8_t *data, uint16_t len)
{
    if (!fields || !report_id || !data || len == 0 || max_fields == 0) return 0;

    memset(fields, 0, (size_t)max_fields * sizeof(hid_field_t));
    *report_id = 0;
    uint8_t num_fields = 0;

    // ---- 全局状态（Global items：Main item 之后保持）----
    uint16_t glb_usage_page  = 0;
    int32_t  glb_log_min     = 0;
    int32_t  glb_log_max     = 0;
    uint8_t  glb_report_size = 0;
    uint8_t  glb_report_count = 0;
    uint16_t bit_offset       = 0;   // 累计位偏移（不含 Report ID 字节）

    // ---- 局部状态（Local items：Main item 之后复位）----
    #define _LOC_MAX_USAGES 8
    uint16_t loc_usages[_LOC_MAX_USAGES];
    uint8_t  loc_usage_count = 0;
    uint32_t loc_usage_min   = 0;
    uint32_t loc_usage_max   = 0;
    bool     loc_has_usage_range = false;

    // Collection 嵌套计数（仅用于跳过，不解析 usage 继承）
    int collection_depth = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        // 长 item（0xFE）：跳过
        if (prefix == 0xFE) {
            if (i >= len) break;
            uint8_t sz = data[i++];
            if (i + sz > len) break;
            i += sz;
            continue;
        }

        uint8_t sz = ITEM_DATA_SIZE(prefix);
        if (i + sz > len) break;

        switch (prefix) {

        /*--- Global items ---*/

        case 0x05:  // Usage Page (1B)
            glb_usage_page = data[i++];
            break;

        case 0x15: case 0x16: case 0x17:  // Logical Minimum
            glb_log_min = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x25: case 0x26: case 0x27:  // Logical Maximum
            glb_log_max = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x75:  // Report Size (1B)
            glb_report_size = data[i++];
            break;

        case 0x85:  // Report ID (1B)
            *report_id = data[i++];
            bit_offset = 0;
            break;

        case 0x95:  // Report Count (1B)
            glb_report_count = data[i++];
            break;

        /*--- Local items ---*/

        case 0x09: {  // Usage (1B)
            uint8_t usage = data[i++];
            if (loc_usage_count < _LOC_MAX_USAGES) {
                loc_usages[loc_usage_count++] = usage;
            }
            break;
        }

        case 0x19: case 0x1A: case 0x1B:  // Usage Minimum
            loc_usage_min = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        case 0x29: case 0x2A: case 0x2B:  // Usage Maximum
            loc_usage_max = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        /*--- Main items ---*/

        case 0x81: {  // Input
            if (i >= len) break;
            uint8_t flags = data[i++];

            uint8_t count = glb_report_count;
            if (count == 0) count = 1;

            bool is_const   = (flags & 0x01) != 0;
            bool is_rel     = (flags & 0x04) != 0;

            if (is_const) {
                bit_offset += (uint16_t)glb_report_size * count;
            } else if (loc_usage_count > 0) {
                for (uint8_t j = 0; j < count && num_fields < max_fields; j++) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = loc_usages[j % loc_usage_count];
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
            } else if (loc_has_usage_range) {
                uint32_t range = loc_usage_max - loc_usage_min + 1;
                for (uint32_t j = 0; j < range && num_fields < max_fields; j++) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = (uint16_t)(loc_usage_min + j);
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
                if ((uint32_t)count > range) {
                    bit_offset += (uint16_t)glb_report_size * (count - (uint8_t)range);
                }
            } else {
                bit_offset += (uint16_t)glb_report_size * count;
            }

            loc_usage_count = 0;
            loc_has_usage_range = false;
            break;
        }

        case 0xA1:  // Collection（Main item：消耗并复位局部状态，
                    // 否则集合顶层的 Usage (Pointer) 等会劫持后续 Input）
            i++;
            collection_depth++;
            loc_usage_count = 0;
            loc_has_usage_range = false;
            break;

        case 0xC0:  // End Collection
            if (collection_depth > 0) collection_depth--;
            break;

        default:
            i += sz;
            break;
        }
    }

    return num_fields;
}

/*--------------------------------------------------------------------+
 * 鼠标描述符解析
 *--------------------------------------------------------------------*/

bool hid_mouse_parse(hid_mouse_desc_t *desc, const uint8_t *data, uint16_t len)
{
    if (!desc || !data || len == 0) return false;

    memset(desc, 0, sizeof(*desc));
    desc->idx_x       = 0xFF;
    desc->idx_y       = 0xFF;
    desc->idx_wheel   = 0xFF;
    desc->idx_buttons = 0xFF;

    // 调用共享字段解析器
    uint8_t report_id = 0;
    desc->num_fields = hid_parse_report_fields(
        desc->fields, HID_MOUSE_MAX_FIELDS, &report_id, data, len);
    if (desc->num_fields == 0) return false;

    desc->report_id = report_id;

    // ---- 鼠标专用：回填预查索引 ----
    for (uint8_t i = 0; i < desc->num_fields; i++) {
        const hid_field_t *f = &desc->fields[i];
        if (f->usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
            switch (f->usage_id) {
            case HID_USAGE_X_AXIS:
                if (desc->idx_x == 0xFF) desc->idx_x = i;
                break;
            case HID_USAGE_Y_AXIS:
                if (desc->idx_y == 0xFF) desc->idx_y = i;
                break;
            case HID_USAGE_WHEEL:
                if (desc->idx_wheel == 0xFF) desc->idx_wheel = i;
                break;
            }
        }
        if (f->usage_page == HID_USAGE_PAGE_BUTTON) {
            if (desc->idx_buttons == 0xFF) desc->idx_buttons = i;
            desc->button_count++;
        }
    }

    return true;
}
/*--------------------------------------------------------------------+
 * 字段值提取
 *--------------------------------------------------------------------*/

int32_t hid_field_read(const hid_field_t *f,
                       const uint8_t *report, uint16_t report_len)
{
    if (!f || !report || f->bit_size == 0 || f->bit_size > 32)
        return 0;

    // 检查字段是否在报告范围内（uint32 防止大报告 bit_offset 回绕）
    uint32_t end_bit = (uint32_t)f->bit_offset + f->bit_size;
    uint32_t end_byte = (end_bit + 7) / 8;
    if (end_byte > report_len)
        return 0;

    uint16_t byte_idx = f->bit_offset / 8;
    uint8_t  bit_pos  = f->bit_offset % 8;

    // 逐位读取，避免跨多字节时的大移位操作
    uint32_t raw = 0;
    uint8_t bits_read = 0;
    while (bits_read < f->bit_size) {
        uint8_t bits_in_this_byte = 8 - bit_pos;
        if (bits_in_this_byte > f->bit_size - bits_read)
            bits_in_this_byte = f->bit_size - bits_read;
        if (byte_idx >= report_len)
            break;

        uint8_t mask = (uint8_t)((1u << bits_in_this_byte) - 1u);
        uint8_t val = (report[byte_idx] >> bit_pos) & mask;
        raw |= (uint32_t)val << bits_read;

        bits_read += bits_in_this_byte;
        byte_idx++;
        bit_pos = 0;
    }

    // 符号扩展：如果 logical_min < 0 且最高位为 1
    if (f->logical_min < 0) {
        uint32_t sign_bit = (uint32_t)1 << (f->bit_size - 1);
        if (raw & sign_bit) {
            // 高位补 1；bit_size==32 时字段已满 32 位，无需扩展（<<32 是 UB）
            uint32_t sign_ext = (f->bit_size >= 32) ? 0u : (~((uint32_t)0) << f->bit_size);
            raw |= sign_ext;
        }
    }

    return (int32_t)raw;
}

/*--------------------------------------------------------------------+
 * 鼠标报告 dispatch
 *--------------------------------------------------------------------*/

void HIDKIT_HOT(hid_mouse_dispatch)(int8_t slot, hid_mouse_dev_t *dev,
                        const uint8_t *report, uint16_t len)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev || !report || len == 0) return;

    const hid_mouse_desc_t *desc = &dev->desc;

    // Report ID 校验
    const uint8_t *field_data = report;
    uint16_t field_len = len;
    if (desc->report_id != 0) {
        if (len < 1 || report[0] != desc->report_id) return;
        field_data = report + 1;
        field_len = len - 1;
    }

    // ---- 按键边沿检测 ----
    // 注：下面两处以 8 为上限是 uint8_t 位掩码的宽度，不是 HIDKIT_MAX_MOUSE_BUTTONS
    // —— 描述符驱动的多键鼠标不能按后者截断，否则 6 键以上鼠标会丢键。
    if (desc->idx_buttons != 0xFF && desc->button_count > 0) {
        uint8_t buttons = 0;
        for (uint8_t i = 0; i < desc->button_count && i < 8; i++) {
            const hid_field_t *f = &desc->fields[desc->idx_buttons + i];
            if (hid_field_read(f, field_data, field_len)) {
                buttons |= (uint8_t)(1u << i);
            }
        }

        if (dev->initialized) {
            uint8_t changed = buttons ^ dev->last_buttons;
            for (uint8_t bit = 0; bit < 8; bit++) {
                if (changed & (1u << bit)) {
                    // hidkit：改用统一出口（鼠标段前缀 + 按键序号）
                    hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | bit),
                                    (buttons >> bit) & 1);
                }
            }
        }
        dev->last_buttons = buttons;
    }

    // ---- 轴移动 ----
    int16_t x = 0, y = 0, wheel = 0;
    if (desc->idx_x != 0xFF)
        x = (int16_t)hid_field_read(&desc->fields[desc->idx_x], field_data, field_len);
    if (desc->idx_y != 0xFF)
        y = (int16_t)hid_field_read(&desc->fields[desc->idx_y], field_data, field_len);
    if (desc->idx_wheel != 0xFF)
        wheel = (int16_t)hid_field_read(&desc->fields[desc->idx_wheel], field_data, field_len);

    if (x != 0 || y != 0 || wheel != 0) {
        // hidkit：改用统一出口
        hidkit_emit_mouse_abs(slot, x, y, wheel);
    }

    dev->initialized = true;
}

/*--------------------------------------------------------------------+
 * NKRO 键盘描述符解析 + dispatch
 *
 * 不复用 hid_parse_report_fields：它把 usage 范围逐键展开成 hid_field_t
 * （104 键 > HID_PARSE_MAX_FIELDS，且溢出时 bit_offset 停止累加导致后续
 * 字段偏移全错）。NKRO dispatch 只需要"位段 + 起始 keycode"，故这里
 * 用同款 item 走查骨架单独记录 span。
 *
 * 全部函数用 HIDKIT_HOT 标注（Pico 上展开为 __not_in_flash_func 入 RAM）：
 * dispatch/key_edge 跑在输入热路径上，主核写 flash 期间 XIP 停摆，
 * 留在 flash 会在这段窗口拖住 NKRO 报文处理。
 *--------------------------------------------------------------------*/

// 追加一个键位段；段满时丢弃（超出 HID_NKRO_MAX_SPANS 的段极罕见）
static void HIDKIT_HOT(nkro_append_span)(hid_nkro_desc_t *desc, uint8_t report_id,
                             uint16_t bit_offset, uint8_t usage_min,
                             uint8_t count, uint8_t bit_size)
{
    if (desc->num_spans >= HID_NKRO_MAX_SPANS) return;
    hid_nkro_span_t *s = &desc->spans[desc->num_spans++];
    s->report_id  = report_id;
    s->bit_offset = bit_offset;
    s->usage_min  = usage_min;
    s->count      = count;
    s->bit_size   = bit_size;
}

bool HIDKIT_HOT(hid_nkro_parse)(hid_nkro_desc_t *desc, const uint8_t *data, uint16_t len)
{
    if (!desc || !data || len == 0) return false;
    memset(desc, 0, sizeof(*desc));

    // ---- 全局状态 ----
    uint16_t glb_usage_page   = 0;
    uint8_t  glb_report_size  = 0;
    uint8_t  glb_report_count = 0;
    uint8_t  cur_report_id    = 0;
    uint16_t bit_offset       = 0;   // 累计位偏移（不含 Report ID 字节）

    // ---- 局部状态（每个 Main item 消耗后复位，含 Collection）----
    #define _NKRO_MAX_USAGES 8
    uint16_t loc_usages[_NKRO_MAX_USAGES];
    uint8_t  loc_usage_count = 0;
    uint32_t loc_usage_min   = 0;
    uint32_t loc_usage_max   = 0;
    bool     loc_has_usage_range = false;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        // 长 item（0xFE）：跳过
        if (prefix == 0xFE) {
            if (i >= len) break;
            uint8_t sz = data[i++];
            if (i + sz > len) break;
            i += sz;
            continue;
        }

        uint8_t sz = ITEM_DATA_SIZE(prefix);
        if (i + sz > len) break;

        switch (prefix) {

        case 0x05:  // Usage Page (1B)
            glb_usage_page = data[i++];
            break;

        case 0x75:  // Report Size (1B)
            glb_report_size = data[i++];
            break;

        case 0x85:  // Report ID (1B)：各 Report 的位偏移空间独立
            cur_report_id = data[i++];
            bit_offset = 0;
            break;

        case 0x95:  // Report Count (1B)
            glb_report_count = data[i++];
            break;

        case 0x09:  // Usage (1B)
            if (loc_usage_count < _NKRO_MAX_USAGES) {
                loc_usages[loc_usage_count++] = data[i++];
            } else {
                i++;
            }
            break;

        case 0x19: case 0x1A: case 0x1B:  // Usage Minimum
            loc_usage_min = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        case 0x29: case 0x2A: case 0x2B:  // Usage Maximum
            loc_usage_max = read_unsigned(&data[i], sz);
            loc_has_usage_range = true;
            i += sz;
            break;

        case 0x81: {  // Input
            if (i >= len) break;
            uint8_t flags = data[i++];

            uint16_t count = glb_report_count ? glb_report_count : 1;

            if (flags & HID_INPUT_CONSTANT) {
                bit_offset += (uint16_t)glb_report_size * count;
                break;
            }
            if (glb_usage_page != HID_USAGE_PAGE_KEYBOARD) {
                // 非 Keyboard/Keypad 页（Consumer/系统控制等）：不解析，只推进偏移
                bit_offset += (uint16_t)glb_report_size * count;
                break;
            }

            if (glb_report_size == 1) {
                // 位图段：逐位推导 keycode（显式 usage 列表或 usage 范围），
                // 合并连续 keycode 为 span（范围里有保留键等空洞时自然分段）
                uint16_t run_off = 0;
                uint8_t  run_min = 0, run_len = 0;
                for (uint16_t j = 0; j < count; j++) {
                    uint32_t kc;
                    if (loc_usage_count > 0) {
                        // HID 规范：usage 列表耗尽后其余字段沿用最后 usage
                        kc = (j < loc_usage_count)
                             ? loc_usages[j] : loc_usages[loc_usage_count - 1];
                    } else if (loc_has_usage_range) {
                        uint32_t range = loc_usage_max - loc_usage_min + 1;
                        if ((uint32_t)j >= range) break;  // 超出范围的位是 padding
                        kc = loc_usage_min + j;
                    } else {
                        break;
                    }
                    // 0x00-0x03 为 ErrorRollOver 等保留键，视作空洞不产生事件
                    bool gap = (kc < HID_NKRO_KEY_MIN || kc > 0xFF);
                    if (!gap && run_len > 0 && kc == run_min + run_len) {
                        run_len++;
                    } else {
                        if (run_len) {
                            nkro_append_span(desc, cur_report_id, run_off,
                                             run_min, run_len, 1);
                        }
                        if (!gap) {
                            run_off = bit_offset + j;
                            run_min = (uint8_t)kc;
                            run_len = 1;
                        } else {
                            run_len = 0;
                        }
                    }
                }
                if (run_len) {
                    nkro_append_span(desc, cur_report_id, run_off,
                                     run_min, run_len, 1);
                }
            } else if (glb_report_size == 8) {
                // 数组段：字节值即 keycode（6KRO 键值数组 / Win8 全键 Rollover）
                uint16_t slots = count > HID_NKRO_MAX_ARRAY ? HID_NKRO_MAX_ARRAY : count;
                nkro_append_span(desc, cur_report_id, bit_offset, 0,
                                 (uint8_t)slots, 8);
            }
            // 其他 Report Size 的 0x07 字段不支持，只推进偏移
            bit_offset += (uint16_t)glb_report_size * count;

            loc_usage_count = 0;
            loc_has_usage_range = false;
            break;
        }

        case 0xA1:  // Collection（Main item：消耗并复位局部状态，
                    // 否则集合顶层的 Usage (Keyboard) 等会劫持后续 Input）
            i++;
            loc_usage_count = 0;
            loc_has_usage_range = false;
            break;

        default:  // Output/Feature 等：不推进 Input 偏移空间，只跳过数据
            i += sz;
            break;
        }
    }

    return desc->num_spans > 0;
}

// 单键边沿检测 + shadow 更新（所有段共用一份 shadow，按 keycode 索引；
// 位图段与数组段、跨 Report ID 的重复键都由此去重）
static void HIDKIT_HOT(nkro_key_edge)(int8_t slot, hid_nkro_dev_t *dev, uint16_t keycode, bool down)
{
    if (keycode < HID_NKRO_KEY_MIN || keycode > 0xFF) return;

    uint8_t mask  = (uint8_t)(1u << (keycode & 7));
    uint8_t *cell = &dev->last_keys[keycode >> 3];
    if (down) {
        if (!(*cell & mask)) {
            *cell |= mask;
            // hidkit：改用统一出口（键盘段前缀 + HID Usage ID）
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | keycode), true);
        }
    } else {
        if (*cell & mask) {
            *cell &= (uint8_t)~mask;
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | keycode), false);
        }
    }
}

void HIDKIT_HOT(hid_nkro_dispatch)(int8_t slot, hid_nkro_dev_t *dev, const uint8_t *report, uint16_t len)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev || !report || len == 0 || dev->desc.num_spans == 0) return;

    for (uint8_t s = 0; s < dev->desc.num_spans; s++) {
        const hid_nkro_span_t *sp = &dev->desc.spans[s];

        // Report ID 校验（按段各自的 ID，兼容同接口多 Report ID 的描述符）
        if (sp->report_id != 0 && (len < 1 || report[0] != sp->report_id)) {
            continue;
        }
        const uint8_t *body     = report + (sp->report_id ? 1 : 0);
        uint16_t       body_len = len - (sp->report_id ? 1 : 0);

        if (sp->bit_size == 1) {
            // 位图段：第 j 位 → keycode = usage_min + j
            for (uint8_t j = 0; j < sp->count; j++) {
                uint32_t bit = (uint32_t)sp->bit_offset + j;
                if ((bit >> 3) >= body_len) break;   // 段超出报文长度（防越界）
                bool down = (body[bit >> 3] >> (bit & 7)) & 1;
                nkro_key_edge(slot, dev, sp->usage_min + j, down);
            }
        } else {
            // 数组段：第 j 槽的字节值即 keycode（非字节对齐时走通用位域读取）
            for (uint8_t j = 0; j < sp->count; j++) {
                hid_field_t f = { .bit_offset = (uint16_t)(sp->bit_offset + 8u * j),
                                  .bit_size = 8, .logical_min = 0 };
                uint8_t kc = (uint8_t)hid_field_read(&f, body, body_len);
                nkro_key_edge(slot, dev, kc, kc != 0);
            }
        }
    }
}

void HIDKIT_HOT(hid_nkro_release_all)(int8_t slot, hid_nkro_dev_t *dev)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev) return;
    for (uint16_t kc = 0; kc < 256; kc++) {
        if (dev->last_keys[kc >> 3] & (1u << (kc & 7))) {
            // hidkit：改用统一出口（键盘段前缀 + HID Usage ID）
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | kc), false);
        }
    }
    memset(dev->last_keys, 0, sizeof(dev->last_keys));
}
