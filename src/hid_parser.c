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
    uint8_t  cur_report_id    = 0;   // 当前 Report ID：字段各自记录（多 ID 描述符）
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

        case 0x05: case 0x06:  // Usage Page（1B / 2B）
            glb_usage_page = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x07:  // Usage Page (4B)：不支持该宽度 → 按规范复位，不沿用上一个 item 的旧值
            glb_usage_page = 0;
            i += sz;
            break;

        case 0x15: case 0x16: case 0x17:  // Logical Minimum
            glb_log_min = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x25: case 0x26: case 0x27:  // Logical Maximum
            glb_log_max = read_signed(&data[i], sz);
            i += sz;
            break;

        case 0x75: case 0x76:  // Report Size（1B / 2B）
            glb_report_size = (uint8_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x77:  // Report Size (4B)：不支持 → 复位
            glb_report_size = 0;
            i += sz;
            break;

        case 0x85: case 0x86: {  // Report ID（1B / 2B）
            // Report ID 规范上 ≤ 255：2 字节形式的高字节非零属非法，按"无 ID"处理
            uint32_t rid = read_unsigned(&data[i], sz);
            cur_report_id = (rid <= 0xFFu) ? (uint8_t)rid : 0;
            // 主 Report ID 取首个非零（多 ID 描述符里各字段见 hid_field_t.report_id）
            if (cur_report_id != 0 && *report_id == 0) *report_id = cur_report_id;
            i += sz;
            bit_offset = 0;      // 每个 Report ID 的位偏移空间独立
            break;
        }

        case 0x87:  // Report ID (4B)：不支持 → 复位为"无 ID"
            cur_report_id = 0;
            i += sz;
            bit_offset = 0;
            break;

        case 0x95: case 0x96:  // Report Count（1B / 2B）
            glb_report_count = (uint8_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x97:  // Report Count (4B)：不支持 → 复位
            glb_report_count = 0;
            i += sz;
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
                    // HID 规范：usage 列表比 Report Count 少时，其余字段**重复最后一个** usage
                    // （原来是 j % loc_usage_count 从头环绕复用，会把轴认成按键；已与
                    //  NKRO 路径的语义对齐）
                    uint8_t ui = (j < loc_usage_count) ? j : (uint8_t)(loc_usage_count - 1);
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = loc_usages[ui];
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->report_id   = cur_report_id;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
            } else if (loc_has_usage_range) {
                uint32_t range = loc_usage_max - loc_usage_min + 1;
                // HID 规范：字段数由 Report Count 决定 —— 循环上界必须是 min(count, range)。
                // 原来直接用 range，count < range 时 bit_offset 会多推进，
                // 使**该字段之后的所有字段偏移全部错位**（与 NKRO 路径一致：多出的
                // usage 视作 padding，不产生字段）
                uint32_t n = ((uint32_t)count < range) ? (uint32_t)count : range;
                for (uint32_t j = 0; j < n && num_fields < max_fields; j++) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = glb_usage_page;
                    f->usage_id    = (uint16_t)(loc_usage_min + j);
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->report_id   = cur_report_id;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                    bit_offset += glb_report_size;
                }
                if ((uint32_t)count > n) {
                    // range 之外的位仍是本字段的一部分（无 usage 可归），只推进偏移
                    bit_offset += (uint16_t)glb_report_size * (uint8_t)((uint32_t)count - n);
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

        case 0xC0:  // End Collection（Main item：同样要复位局部状态）
            if (collection_depth > 0) collection_depth--;
            // HID 规范：End Collection 清空局部 usage/usage-min/max，
            // 否则集合结束后新开的 Input 会继承上一个集合的 usage 归属
            loc_usage_count = 0;
            loc_has_usage_range = false;
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
            // 按键字段逐个记下标（多 Report ID 描述符里按键可能不与轴连续排列，
            // 原来的"起始字段 + 个数"会连带把轴字段当成按键读）
            if (desc->button_count < HID_MOUSE_MAX_BTNS) {
                desc->btn_idx[desc->button_count++] = i;
            }
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

// 按字段自带的 Report ID 定位本次报文的数据体（不含 Report ID 字节）：
// 字段不属于本报文时返回 NULL —— 多 Report ID 描述符里只有 ID 相符的那部分字段
// 会随本报文更新（原来只认描述符最后一个 ID，其余 Report 的字段全读不到）
static const uint8_t *mouse_field_body(const hid_field_t *f, const uint8_t *report,
                                       uint16_t len, uint16_t *body_len)
{
    if (f->report_id != 0) {
        if (len < 1 || report[0] != f->report_id) return NULL;
        *body_len = (uint16_t)(len - 1);
        return report + 1;
    }
    *body_len = len;   // 无 Report ID：整份报文即数据体
    return report;
}

void HIDKIT_HOT(hid_mouse_dispatch)(int8_t slot, hid_mouse_dev_t *dev,
                        const uint8_t *report, uint16_t len)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev || !report || len == 0) return;

    const hid_mouse_desc_t *desc = &dev->desc;

    // ---- 按键边沿检测 ----
    // 注：下面两处以 8 为上限是 uint8_t 位掩码的宽度，不是 HIDKIT_MAX_MOUSE_BUTTONS
    // —— 描述符驱动的多键鼠标不能按后者截断，否则 6 键以上鼠标会丢键。
    if (desc->button_count > 0) {
        uint8_t buttons = 0;
        for (uint8_t i = 0; i < desc->button_count && i < HID_MOUSE_MAX_BTNS; i++) {
            const hid_field_t *f = &desc->fields[desc->btn_idx[i]];
            uint16_t blen;
            const uint8_t *body = mouse_field_body(f, report, len, &blen);
            // 字段不属于本报文时保持上次状态，避免别的 Report ID 的报文把它误判成"松开"
            bool on = body ? (hid_field_read(f, body, blen) != 0)
                           : ((dev->last_buttons >> i) & 1u) != 0;
            if (on) buttons |= (uint8_t)(1u << i);
        }

        if (dev->initialized) {
            uint8_t changed = buttons ^ dev->last_buttons;
            for (uint8_t bit = 0; bit < HID_MOUSE_MAX_BTNS; bit++) {
                if (changed & (1u << bit)) {
                    // hidkit：改用统一出口（鼠标段前缀 + 按键序号）
                    hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | bit),
                                    (buttons >> bit) & 1);
                }
            }
        }
        dev->last_buttons = buttons;
    }

    // ---- 轴移动（X / Y / Wheel 各按自己的 Report ID 取数据）----
    const uint8_t axis_idx[3] = { desc->idx_x, desc->idx_y, desc->idx_wheel };
    int16_t axis[3] = { 0, 0, 0 };
    for (uint8_t a = 0; a < 3; a++) {
        if (axis_idx[a] == 0xFF) continue;
        uint16_t blen;
        const uint8_t *body = mouse_field_body(&desc->fields[axis_idx[a]], report, len, &blen);
        if (body) axis[a] = (int16_t)hid_field_read(&desc->fields[axis_idx[a]], body, blen);
    }

    if (axis[0] != 0 || axis[1] != 0 || axis[2] != 0) {
        // hidkit：改用统一出口
        hidkit_emit_mouse_abs(slot, axis[0], axis[1], axis[2]);
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

        case 0x05: case 0x06:  // Usage Page（1B / 2B）
            glb_usage_page = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x07:  // Usage Page (4B)：不支持 → 复位（与通用解析器同规则）
            glb_usage_page = 0;
            i += sz;
            break;

        case 0x75: case 0x76:  // Report Size（1B / 2B）
            glb_report_size = (uint8_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x77:  // Report Size (4B)：不支持 → 复位
            glb_report_size = 0;
            i += sz;
            break;

        case 0x85: case 0x86: {  // Report ID（1B / 2B）：各 Report 的位偏移空间独立
            uint32_t rid = read_unsigned(&data[i], sz);
            cur_report_id = (rid <= 0xFFu) ? (uint8_t)rid : 0;
            i += sz;
            bit_offset = 0;
            break;
        }

        case 0x87:  // Report ID (4B)：不支持 → 复位为"无 ID"
            cur_report_id = 0;
            i += sz;
            bit_offset = 0;
            break;

        case 0x95: case 0x96:  // Report Count（1B / 2B）
            glb_report_count = (uint8_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x97:  // Report Count (4B)：不支持 → 复位
            glb_report_count = 0;
            i += sz;
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

        case 0xC0:  // End Collection（同为 Main item：按规范清局部状态，
                    // 否则集合结束后的 Input 会继承上一个集合的 usage）
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
