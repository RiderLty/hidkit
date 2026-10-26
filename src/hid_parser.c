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
    // HID 的 item 只有 0/1/2/4 字节四种数据宽度（bSize=3 → 4 字节），
    // 4 字节的 raw 本身就是完整 int32 位型，直接转换即为正确的二补数。
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

// ------------------------------------------------------------------
// Push(0xA4) / Pop(0xB4) 的全局状态栈
//
// 规范里 Push/Pop 保存/恢复的是**全局 item 状态**（Usage Page / Report Size /
// Report Count / Report ID / Logical Min/Max），不包含位偏移（那是解析位置，
// 不是全局量）。hidrd 与 Linux hid-core 都实现了它；此前本库只跳过这两个 item，
// 于是 "Push → 改 Usage Page → Pop" 的描述符在 Pop 之后仍沿用被改过的页，
// 后续字段归属错乱。深度取 4：真实描述符几乎不会更深。
// ------------------------------------------------------------------
#define HID_GLOBAL_STACK_MAX 4

typedef struct {
    uint16_t usage_page;
    uint16_t report_count;
    uint8_t  report_size;
    uint8_t  report_id;
    int32_t  log_min;
    int32_t  log_max;
} hid_global_state_t;


/*--------------------------------------------------------------------+
 * 通用描述符字段解析（共享 — mouse/gamepad 共用）
 *--------------------------------------------------------------------*/

uint8_t hid_parse_report_fields_filtered(hid_field_t *fields, uint8_t max_fields,
                                         uint8_t *report_id,
                                         const uint8_t *data, uint16_t len,
                                         hid_field_keep_fn keep)
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
    uint16_t glb_report_count = 0;
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

    // Push/Pop 全局状态栈
    hid_global_state_t gstack[HID_GLOBAL_STACK_MAX];
    uint8_t gstack_n = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        // 长 item（0xFE）：格式是 FE <bDataSize> <bLongItemTag> <data...>，
        // 标签字节也是 item 的一部分，漏掉它会把它当成下一个 item 的前缀，
        // 此后整份描述符的页/宽度/偏移全部错位。
        if (prefix == 0xFE) {
            if (i + 2 > len) break;       // 需要 bDataSize + bLongItemTag
            uint8_t sz = data[i];         // i 指向 bDataSize
            if ((uint32_t)i + 2u + sz > len) break;
            i = (uint16_t)(i + 2u + sz);  // 跳过 tag 与 sz 个数据字节
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
            glb_report_count = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x97:  // Report Count (4B)：不支持 → 复位
            glb_report_count = 0;
            i += sz;
            break;

        /*--- Local items ---*/

        case 0x09: case 0x0A: case 0x0B: {  // Usage（1B / 2B / 4B）
            // 之前只认 1 字节形式，0x0A/0x0B（规范合法写法，工具生成的描述符
            // 常用）落进 default：字段的 usage 全部丢失，鼠标被认领却解析不出
            // X/Y（hid_report 恒返回已消费、却不出任何事件）。
            uint32_t usage = read_unsigned(&data[i], sz);
            if (loc_usage_count < _LOC_MAX_USAGES) {
                loc_usages[loc_usage_count++] = usage;
            }
            i += sz;
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

            // HID 规范：Report Count = 0 的 main item 不产生任何字段、也不占位
            // （全局量默认值就是 0，"Report Count 0 + Input" 是合法的空操作写法）。
            uint16_t count = glb_report_count;

            bool is_const   = (flags & 0x01) != 0;
            bool is_rel     = (flags & 0x04) != 0;

            if (is_const) {
                bit_offset += (uint16_t)glb_report_size * count;
                loc_usage_count = 0;
                loc_has_usage_range = false;
                break;
            }

            // 逐位字段的 usage 归属：
            //   - usage 列表：报告数多于列表时**重复最后一个** usage
            //   - usage 范围：字段数取 min(count, range)，range 之外的位是 padding
            // keep() 为 NULL 或返回 true 的字段才落进 fields[]；无论是否收录，
            // bit_offset 都按 Report Size 逐位推进（被过滤/触顶都不会让后续偏移错位）。
            bool use_list  = (loc_usage_count > 0);
            bool use_range = (!use_list && loc_has_usage_range);
            uint32_t n = 0;
            if (use_list) {
                n = count;
            } else if (use_range) {
                uint32_t range = loc_usage_max - loc_usage_min + 1;
                n = ((uint32_t)count < range) ? (uint32_t)count : range;
            }

            for (uint32_t j = 0; j < n; j++) {
                uint16_t page  = glb_usage_page;
                uint16_t usage = use_list
                    ? loc_usages[(j < loc_usage_count) ? j : (uint32_t)(loc_usage_count - 1)]
                    : (uint16_t)(loc_usage_min + j);

                if (num_fields < max_fields && (!keep || keep(page, usage))) {
                    hid_field_t *f = &fields[num_fields++];
                    f->usage_page  = page;
                    f->usage_id    = usage;
                    f->bit_offset  = bit_offset;
                    f->bit_size    = glb_report_size;
                    f->report_id   = cur_report_id;
                    f->logical_min = glb_log_min;
                    f->logical_max = glb_log_max;
                    f->is_relative = is_rel;
                    f->is_constant = false;
                }
                bit_offset += glb_report_size;
            }

            if (use_range) {
                // range 之外的位仍是本字段的一部分（无 usage 可归），只推进偏移
                // 注意别把余数截成 8 位：count=300、range 占 2 项时，
                // 余数 298 会被截断成 42，后续字段偏移整体错位
                if ((uint32_t)count > n) {
                    bit_offset += (uint16_t)glb_report_size *
                                  (uint16_t)((uint32_t)count - n);
                }
            } else if (!use_list) {
                // 无 usage 的 Input（含 Report Count 0）：只推进偏移
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

        case 0xA4:  // Push：保存全局状态
            if (gstack_n < HID_GLOBAL_STACK_MAX) {
                hid_global_state_t *g = &gstack[gstack_n++];
                g->usage_page   = glb_usage_page;
                g->report_count = glb_report_count;
                g->report_size  = glb_report_size;
                g->report_id    = cur_report_id;
                g->log_min      = glb_log_min;
                g->log_max      = glb_log_max;
            }
            i += sz;   // 无数据（sz == 0）
            break;

        case 0xB4:  // Pop：恢复全局状态
            if (gstack_n > 0) {
                const hid_global_state_t *g = &gstack[--gstack_n];
                glb_usage_page   = g->usage_page;
                glb_report_count = g->report_count;
                glb_report_size  = g->report_size;
                cur_report_id    = g->report_id;
                glb_log_min      = g->log_min;
                glb_log_max      = g->log_max;
            }
            i += sz;
            break;

        default:
            // 其余 Main item（Output / Feature / Collection 之外的 0xBx 等）同样
            // **消费局部 usage**（HID 1.11 §6.2.2.8）。之前只跳过数据，局部
            // usage 会残留到下一个 Input：媒体键 Input 之后紧接
            // `19 E0 29 E7` 修饰键范围时，残留的 Usage(0xE9) 被当作 usage 列表，
            // 8 位修饰键位图被解成 8 个假的 0xE9。
            if ((prefix & 0xFC) == 0x80 || (prefix & 0xFC) == 0x90 ||
                (prefix & 0xFC) == 0xB0) {
                loc_usage_count = 0;
                loc_has_usage_range = false;
            }
            i += sz;
            break;
        }
    }

    return num_fields;
}

uint8_t hid_parse_report_fields(hid_field_t *fields, uint8_t max_fields,
                                 uint8_t *report_id,
                                 const uint8_t *data, uint16_t len)
{
    return hid_parse_report_fields_filtered(fields, max_fields, report_id,
                                            data, len, NULL);
}

/*--------------------------------------------------------------------+
 * 报文长度表（描述符 → 每个 Report ID 的输入报文数据体位宽）
 *
 * 描述符是报文长度的权威来源：Report Size × Report Count 累加起来是多少位，
 * 设备在固定长度的中断端点上就发多少字节（Report ID 另占 1 字节）。
 * Linux hid-core 用同一判据（`report->size` 位 → `hid_compute_report_size()`
 * 字节，不含 ID 字节）在 `hid_report_raw_event()` 里拒收"比描述符短"的报文：
 * 新版本返回 -EINVAL 并打 "Event data for report %d was too short"，
 * 旧版本只打 "report %d is too short" 并补零。本表就是它的移植
 * （只做前者：短报文整份拒绝，不补零 —— 补零会让缺失字段读成 0，
 * 在相对轴上是"没动"、在按键上是"松开"，都是凭空造事件）。
 *
 * 为什么必须校验整份报文长度：字段级的越界检查只保证"这个字段读得到"，
 * 读得到不等于读得对。真机上鼠标接口收到 2 字节杂报文（BE EF），X 轴字段
 * （11 字节布局里的 bit 8..23）恰好落在其中 → 解出 dx = -4162 的假位移，
 * 每帧 187 次，上层表现为"没动鼠标但光标一直飞"。
 *
 * 除总位宽外还按**最外层 Application Collection** 分三类累计（键盘/鼠标/
 * 其它）。用途不是路由（无 Report ID 时键鼠共用一份报文，不能按集合拆），
 * 而是给"描述符里有没有键盘集合"这类判定提供依据 —— boot 兜底要靠它确认
 * 一个谎报 proto=Keyboard 的接口究竟是不是键盘。
 *--------------------------------------------------------------------*/

enum { COLL_OTHER = 0, COLL_KB, COLL_MOUSE };

static int coll_class(uint16_t page, uint32_t usage)
{
    if (page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
        (usage == HID_USAGE_DESKTOP_POINTER || usage == HID_USAGE_DESKTOP_MOUSE)) {
        return COLL_MOUSE;
    }
    if ((page == HID_USAGE_PAGE_GENERIC_DESKTOP && usage == HID_USAGE_DESKTOP_KEYBOARD) ||
        (page == HID_USAGE_PAGE_KEYBOARD && usage == HID_USAGE_DESKTOP_KEYBOARD)) {
        return COLL_KB;
    }
    return COLL_OTHER;
}

// 往表里累加一份 Input 的位宽；同 ID 的多次 Input 累加，跨集合的总量单独记
static void lens_add(hid_report_len_t *lens, uint8_t *n, uint8_t max,
                     uint8_t id, uint32_t bits, int klass, bool *overflow)
{
    if (bits == 0) return;

    hid_report_len_t *e = NULL;
    for (uint8_t k = 0; k < *n; k++) {
        if (lens[k].id == id) { e = &lens[k]; break; }
    }
    if (!e) {
        if (*n >= max) {          // 表满：记下溢出，该 ID 的报文一律不接受
            if (overflow) *overflow = true;
            return;
        }
        e = &lens[(*n)++];
        e->id = id;
        e->bits = 0;
        e->bits_kb = 0;
        e->bits_mouse = 0;
    }

    uint32_t v = (uint32_t)e->bits + bits;
    e->bits = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
    if (klass == COLL_KB) {
        v = (uint32_t)e->bits_kb + bits;
        e->bits_kb = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
    } else if (klass == COLL_MOUSE) {
        v = (uint32_t)e->bits_mouse + bits;
        e->bits_mouse = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
    }
}

// 带溢出标志的内部版本：descriptor 的 Report ID 多于表容量时置 *overflow
static uint8_t HIDKIT_HOT(lens_scan)(const uint8_t *data, uint16_t len,
                                     hid_report_len_t *out, uint8_t max_out,
                                     bool *overflow)
{
    if (!data || len == 0 || !out || max_out == 0) return 0;
    memset(out, 0, (size_t)max_out * sizeof(hid_report_len_t));

    uint8_t num_out = 0;

    // ---- 全局状态（与字段解析器同规则：不支持的宽度一律复位）----
    uint16_t glb_page  = 0;
    uint32_t glb_size  = 0;
    uint32_t glb_count = 0;
    uint8_t  cur_id    = 0;

    // ---- 局部状态：只需要"最近一个 Usage"给 Collection 用 ----
    uint32_t loc_usage = 0;
    bool     loc_has_usage = false;

    // 最外层 Application Collection 的 usage（只关心第 1 层）
    uint16_t coll_page  = 0;
    uint32_t coll_usage = 0;
    int      depth      = 0;

    #define _LEN_STACK 4
    hid_global_state_t gstack[_LEN_STACK];
    uint8_t gstack_n = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        if (prefix == 0xFE) {                 // 长 item：跳过（含 tag 字节）
            if (i + 2 > len) break;       // 需要 bDataSize + bLongItemTag
            uint8_t sz = data[i];         // i 指向 bDataSize
            if ((uint32_t)i + 2u + sz > len) break;
            i = (uint16_t)(i + 2u + sz);  // 跳过 tag 与 sz 个数据字节
            continue;
        }

        uint8_t sz = ITEM_DATA_SIZE(prefix);
        if (i + sz > len) break;

        switch (prefix) {
        case 0x05: case 0x06:                 // Usage Page（1B / 2B）
            glb_page = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;
        case 0x07:                            // Usage Page (4B)：不支持 → 复位
            glb_page = 0;
            i += sz;
            break;

        case 0x75: case 0x76:                 // Report Size（1B / 2B）
            glb_size = read_unsigned(&data[i], sz);
            i += sz;
            break;
        case 0x77:                            // Report Size (4B)：不支持 → 复位
            glb_size = 0;
            i += sz;
            break;

        case 0x95: case 0x96:                 // Report Count（1B / 2B）
            glb_count = read_unsigned(&data[i], sz);
            i += sz;
            break;
        case 0x97:                            // Report Count (4B)：不支持 → 复位
            glb_count = 0;
            i += sz;
            break;

        case 0x85: case 0x86: {               // Report ID（1B / 2B）
            uint32_t rid = read_unsigned(&data[i], sz);
            cur_id = (rid <= 0xFFu) ? (uint8_t)rid : 0;
            i += sz;
            break;
        }
        case 0x87:                            // Report ID (4B)：不支持 → 复位
            cur_id = 0;
            i += sz;
            break;

        case 0x09: case 0x0A: case 0x0B:      // Usage：只留第一个（给 Collection）
            if (!loc_has_usage) {
                loc_usage = read_unsigned(&data[i], sz);
                loc_has_usage = true;
            }
            i += sz;
            break;

        case 0xA1: {                          // Collection
            i++;                              // 集合类型
            if (depth == 0) {
                coll_page  = glb_page;
                coll_usage = loc_has_usage ? loc_usage : 0;
            }
            if (depth < 0x7FFF) depth++;
            loc_has_usage = false;            // Main item 消耗 local
            break;
        }
        case 0xC0:                            // End Collection
            if (depth > 0) depth--;
            loc_has_usage = false;
            break;

        case 0x81: {                          // Input：只有它占报文长度
            if (i >= len) break;
            i++;                              // flags
            uint32_t bits = glb_size * glb_count;

            int klass = COLL_OTHER;
            if (depth > 0) {
                klass = coll_class(coll_page, coll_usage);
            } else if (glb_page == HID_USAGE_PAGE_KEYBOARD) {
                // 没包在任何集合里的键位段（少见，但 AULA 一类厂商描述符有）
                klass = COLL_KB;
            }
            lens_add(out, &num_out, max_out, cur_id, bits, klass, overflow);
            loc_has_usage = false;
            break;
        }

        case 0xA4:                            // Push
            if (gstack_n < _LEN_STACK) {
                hid_global_state_t *g = &gstack[gstack_n++];
                g->usage_page   = glb_page;
                g->report_count = (uint16_t)glb_count;
                g->report_size  = (uint8_t)glb_size;
                g->report_id    = cur_id;
                g->log_min      = 0;
                g->log_max      = 0;
            }
            i += sz;
            break;
        case 0xB4:                            // Pop
            if (gstack_n > 0) {
                const hid_global_state_t *g = &gstack[--gstack_n];
                glb_page  = g->usage_page;
                glb_count = g->report_count;
                glb_size  = g->report_size;
                cur_id    = g->report_id;
            }
            i += sz;
            break;

        default:
            // 其他 Main item（Output/Feature/End Collection 之外的 0x8x/0x9x/0xBx）
            // 也消耗 local usage
            if ((prefix & 0xFC) == 0x80 || (prefix & 0xFC) == 0x90 ||
                (prefix & 0xFC) == 0xB0) {
                loc_has_usage = false;
            }
            i += sz;
            break;
        }
    }

    return num_out;
}

uint8_t hid_desc_report_bits(const uint8_t *data, uint16_t len,
                             hid_report_len_t *out, uint8_t max_out)
{
    return lens_scan(data, len, out, max_out, NULL);
}

/*--------------------------------------------------------------------+
 * 鼠标描述符解析
 *--------------------------------------------------------------------*/

// 扫描描述符的**顶层 Application Collection** usage，判断它是否声明了鼠标。
//
// 为什么要看集合而不是只看 X/Y/Button：手柄的描述符里也有 Generic Desktop
// X/Y + Button 页，只看字段会把未知手柄误判成鼠标。Mouse/Pointer 顶层集合
// （Generic Desktop 页 usage 0x02/0x01）才是有鼠标能力的可靠标志。
bool hid_desc_has_mouse_collection(const uint8_t *data, uint16_t len)
{
    if (!data || len == 0) return false;

    uint16_t glb_page     = 0;
    uint32_t loc_usage    = 0;
    bool     loc_has_usage = false;
    int      depth         = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        if (prefix == 0xFE) {          // 长 item：跳过（含 tag 字节）
            if (i + 2 > len) break;       // 需要 bDataSize + bLongItemTag
            uint8_t sz = data[i];         // i 指向 bDataSize
            if ((uint32_t)i + 2u + sz > len) break;
            i = (uint16_t)(i + 2u + sz);  // 跳过 tag 与 sz 个数据字节
            continue;
        }

        uint8_t sz = ITEM_DATA_SIZE(prefix);
        if (i + sz > len) break;

        switch (prefix) {
        case 0x05: case 0x06: case 0x07:   // Usage Page（1B / 2B / 4B）
            glb_page = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x09: case 0x0A: case 0x0B:   // Usage（1B / 2B / 4B）
            loc_usage    = read_unsigned(&data[i], sz);
            loc_has_usage = true;
            i += sz;
            break;

        case 0xA1: {                       // Collection
            uint8_t type = data[i++];
            if (depth == 0 && type == HID_COLLECTION_APPLICATION && loc_has_usage &&
                glb_page == HID_USAGE_PAGE_GENERIC_DESKTOP &&
                (loc_usage == HID_USAGE_DESKTOP_POINTER ||
                 loc_usage == HID_USAGE_DESKTOP_MOUSE)) {
                return true;
            }
            depth++;
            loc_has_usage = false;         // 集合消费掉 local usage
            break;
        }

        case 0xC0:                         // End Collection
            if (depth > 0) depth--;
            loc_has_usage = false;
            break;

        default:
            // 其他 Main item（Input/Output/Feature）同样消费 local usage
            if ((prefix & 0xFC) == 0x80 || (prefix & 0xFC) == 0x90 ||
                (prefix & 0xFC) == 0xB0) {
                loc_has_usage = false;
            }
            i += sz;
            break;
        }
    }
    return false;
}

// 鼠标只关心两类集合：Generic Desktop（X/Y/Wheel）与 Button。
// 其余集合（厂商 0xFFxx / Consumer / Keyboard 等）只推进位偏移、不占字段槽 ——
// 多集合接口里厂商+键盘集合可能展开上百个字段，不滤掉的话后面的鼠标集合
// 根本轮不到（HID_MOUSE_MAX_FIELDS 触顶，idx_x/y/wheel 全 0xFF）。
static bool mouse_field_keep(uint16_t usage_page, uint16_t usage_id)
{
    (void)usage_id;
    return usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP ||
           usage_page == HID_USAGE_PAGE_BUTTON;
}

bool hid_mouse_parse(hid_mouse_desc_t *desc, const uint8_t *data, uint16_t len)
{
    if (!desc || !data || len == 0) return false;

    memset(desc, 0, sizeof(*desc));
    desc->idx_x       = 0xFF;
    desc->idx_y       = 0xFF;
    desc->idx_wheel   = 0xFF;

    // 鼠标只关心两类集合：Generic Desktop（X/Y/Wheel）与 Button。
    // 跳过它们之外的字段，但不影响长度表 —— 长度表按整份描述符累计。
    uint8_t report_id = 0;
    desc->num_fields = hid_parse_report_fields_filtered(
        desc->fields, HID_MOUSE_MAX_FIELDS, &report_id, data, len,
        mouse_field_keep);
    if (desc->num_fields == 0) return false;

    desc->report_id = report_id;

    // 各 Report ID 的报文长度（含被过滤掉的集合）：报文本体的权威字节数
    desc->num_lens = lens_scan(data, len, desc->lens,
                               HID_REPORT_LEN_TABLE, &desc->lens_overflow);

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

        /* 首帧也走边沿比较：last_buttons 初值为 0，挂载时按键已按住（或首个
         * 报文晚到、期间用户已按下）就能正确上报"按下"。旧实现用 initialized
         * 抑制首帧，结果按下语义永久丢失，松开时反而发出一个没有配对按下的 UP。 */
        uint8_t changed = buttons ^ dev->last_buttons;
        for (uint8_t bit = 0; bit < HID_MOUSE_MAX_BTNS; bit++) {
            if (changed & (1u << bit)) {
                // hidkit：改用统一出口（鼠标段前缀 + 按键序号）
                hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | bit),
                                (buttons >> bit) & 1);
            }
        }
        dev->last_buttons = buttons;
    }

    // ---- 轴移动：逐字段扫描 ----
    // 同一接口可有多个鼠标集合（各自带 Report ID）。按字段自身 usage + Report ID
    // 取数据，不再依赖"每个轴只记第一个字段"的 idx_*（旧限制见 KNOWN_ISSUES）。
    int32_t dx = 0, dy = 0, wheel = 0;
    for (uint8_t i = 0; i < desc->num_fields; i++) {
        const hid_field_t *f = &desc->fields[i];
        if (f->usage_page != HID_USAGE_PAGE_GENERIC_DESKTOP) continue;
        if (f->usage_id != HID_USAGE_X_AXIS && f->usage_id != HID_USAGE_Y_AXIS &&
            f->usage_id != HID_USAGE_WHEEL) {
            continue;
        }

        uint16_t blen;
        const uint8_t *body = mouse_field_body(f, report, len, &blen);
        if (!body) continue;                // 字段不属于本报文 → 本次不参与更新
        int32_t v = hid_field_read(f, body, blen);
        if (f->usage_id == HID_USAGE_X_AXIS)       dx    += v;
        else if (f->usage_id == HID_USAGE_Y_AXIS)  dy    += v;
        else                                       wheel += v;
    }

    if (dx != 0 || dy != 0 || wheel != 0) {
        // hidkit：改用统一出口
        hidkit_emit_mouse_abs(slot, dx, dy, wheel);
    }

}

// 该字段是否参与鼠标事件（X/Y/Wheel 或按键）。
// 用字段自身的 usage 判定，而不是 idx_x/y/wheel 单下标 —— 同一接口可以有多个
// 鼠标集合（各自带 Report ID），第二个集合的轴/按键也要能被路由到。
static bool mouse_field_relevant(const hid_mouse_desc_t *d, uint8_t i)
{
    (void)d;
    const hid_field_t *f = &d->fields[i];
    if (f->usage_page == HID_USAGE_PAGE_BUTTON) return true;
    if (f->usage_page == HID_USAGE_PAGE_GENERIC_DESKTOP) {
        return f->usage_id == HID_USAGE_X_AXIS ||
               f->usage_id == HID_USAGE_Y_AXIS ||
               f->usage_id == HID_USAGE_WHEEL;
    }
    return false;
}

bool HIDKIT_HOT(hid_mouse_accepts)(const hid_mouse_dev_t *dev,
                                   const uint8_t *report, uint16_t len)
{
    return hid_mouse_match(dev, report, len) > 0;
}

// 该 Report ID 在长度表里的条目（NULL = 描述符没声明这个 ID 的长度）
static const hid_report_len_t *HIDKIT_HOT(lens_find)(const hid_report_len_t *lens,
                                                     uint8_t n, uint8_t id)
{
    for (uint8_t k = 0; k < n; k++) {
        if (lens[k].id == id) return &lens[k];
    }
    return NULL;
}

uint8_t HIDKIT_HOT(hid_mouse_match)(const hid_mouse_dev_t *dev,
                                    const uint8_t *report, uint16_t len)
{
    if (!dev) return 0;
    const hid_mouse_desc_t *d = &dev->desc;
    if (d->num_fields == 0) return 0;        // 空描述符：没有任何字段可读

    bool has_id = false;
    for (uint8_t i = 0; i < d->num_fields; i++) {
        if (mouse_field_relevant(d, i) && d->fields[i].report_id != 0) {
            has_id = true;
            break;
        }
    }

    uint8_t id = 0;
    if (has_id) {
        if (!report || len < 1) return 0;
        bool hit = false;
        for (uint8_t i = 0; i < d->num_fields; i++) {
            if (mouse_field_relevant(d, i) && d->fields[i].report_id == report[0]) {
                hit = true;
                break;
            }
        }
        if (!hit) return 0;
        id = report[0];
    } else if (!report || len == 0) {
        return 0;
    }

    uint16_t const body_len = (uint16_t)(len - (has_id ? 1 : 0));

    /* 长度校验：描述符声明这份 Report ID 的输入数据体多少位，报文就得装得下。
     *
     * 用**整份报文**的声明长度（而不是只看鼠标字段的位宽），因为设备永远按
     * 描述符声明的总量发包：11 字节的鼠标报文里 X/Y 只占前 6 字节，拿 6 去比
     * 会把真正的 6 字节杂报文也放进来。 */
    const hid_report_len_t *e = lens_find(d->lens, d->num_lens, id);
    /* 表里没有这个 ID：要么描述符根本没声明它（不该走到这里，ID 已按字段命中），
     * 要么 Report ID 多于表容量（lens_overflow）。后者若当作"长度未声明"放行，
     * 第 9 个 ID 的任意短杂报文又能解出假位移 —— 拒绝，交宿主。 */
    if (!e) return d->lens_overflow ? 0 : 1;
    uint16_t const want = (uint16_t)((e->bits + 7u) / 8u);
    if (want != 0 && body_len < want) return 0;   // 短报文：连声明的布局都装不下
    return (body_len == want) ? 2 : 1;            // 2 = 长度精确吻合
}

bool HIDKIT_HOT(hid_mouse_uses_report_id)(const hid_mouse_dev_t *dev)
{
    if (!dev) return false;
    const hid_mouse_desc_t *d = &dev->desc;
    for (uint8_t i = 0; i < d->num_fields; i++) {
        if (mouse_field_relevant(d, i) && d->fields[i].report_id != 0) return true;
    }
    return false;
}

void HIDKIT_HOT(hid_mouse_release_all)(int8_t slot, hid_mouse_dev_t *dev)
{
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev) return;
    // last_buttons 的 bit i 对应 btn_idx[i]，与 dispatch 的编码一致
    for (uint8_t i = 0; i < HID_MOUSE_MAX_BTNS; i++) {
        if (dev->last_buttons & (uint8_t)(1u << i)) {
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_MOUSE | i), false);
        }
    }
    dev->last_buttons = 0;
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
                             uint16_t count, uint8_t bit_size)
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
    uint16_t glb_report_count = 0;
    uint8_t  cur_report_id    = 0;
    uint16_t bit_offset       = 0;   // 累计位偏移（不含 Report ID 字节）

    // ---- 局部状态（每个 Main item 消耗后复位，含 Collection）----
    #define _NKRO_MAX_USAGES 8
    uint16_t loc_usages[_NKRO_MAX_USAGES];
    uint8_t  loc_usage_count = 0;
    uint32_t loc_usage_min   = 0;
    uint32_t loc_usage_max   = 0;
    bool     loc_has_usage_range = false;

    // Push/Pop 全局状态栈（与通用解析器一致；NKRO 侧无 Logical Min/Max）
    hid_global_state_t gstack[HID_GLOBAL_STACK_MAX];
    uint8_t gstack_n = 0;

    for (uint16_t i = 0; i < len; ) {
        uint8_t prefix = data[i++];

        // 长 item（0xFE）：格式是 FE <bDataSize> <bLongItemTag> <data...>，
        // 标签字节也是 item 的一部分，漏掉它会把它当成下一个 item 的前缀，
        // 此后整份描述符的页/宽度/偏移全部错位。
        if (prefix == 0xFE) {
            if (i + 2 > len) break;       // 需要 bDataSize + bLongItemTag
            uint8_t sz = data[i];         // i 指向 bDataSize
            if ((uint32_t)i + 2u + sz > len) break;
            i = (uint16_t)(i + 2u + sz);  // 跳过 tag 与 sz 个数据字节
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
            glb_report_count = (uint16_t)read_unsigned(&data[i], sz);
            i += sz;
            break;

        case 0x97:  // Report Count (4B)：不支持 → 复位
            glb_report_count = 0;
            i += sz;
            break;

        case 0x09: case 0x0A: case 0x0B:  // Usage（1B / 2B / 4B）
            if (loc_usage_count < _NKRO_MAX_USAGES) {
                loc_usages[loc_usage_count++] = read_unsigned(&data[i], sz);
            }
            i += sz;
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

            // 同上（缺陷 6）：Report Count = 0 不产生字段、不占位
            uint16_t count = glb_report_count;

            if (flags & HID_INPUT_CONSTANT) {
                bit_offset += (uint16_t)glb_report_size * count;
                break;
            }
            if (glb_usage_page != HID_USAGE_PAGE_KEYBOARD) {
                // 非 Keyboard/Keypad 页（Consumer/系统控制等）：不解析，只推进偏移。
                // 但局部 usage 仍要被这个 Main item 消费掉 —— 否则残留的
                // Usage(0xE9) 会覆盖紧随其后的 `19 E0 29 E7` 修饰键范围，
                // 8 位修饰键位图被解成 8 个假的 0xE9（真机：按 Shift 出音量键）。
                bit_offset += (uint16_t)glb_report_size * count;
                loc_usage_count = 0;
                loc_has_usage_range = false;
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
                                 slots, 8);
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

        case 0xA4:  // Push：保存全局状态（Usage Page/Size/Count/Report ID）
            if (gstack_n < HID_GLOBAL_STACK_MAX) {
                hid_global_state_t *g = &gstack[gstack_n++];
                g->usage_page   = glb_usage_page;
                g->report_count = glb_report_count;
                g->report_size  = glb_report_size;
                g->report_id    = cur_report_id;
                g->log_min      = 0;
                g->log_max      = 0;
            }
            i += sz;
            break;

        case 0xB4:  // Pop：恢复全局状态
            if (gstack_n > 0) {
                const hid_global_state_t *g = &gstack[--gstack_n];
                glb_usage_page   = g->usage_page;
                glb_report_count = g->report_count;
                glb_report_size  = g->report_size;
                cur_report_id    = g->report_id;
            }
            i += sz;
            break;

        default:
            // Output/Feature 等：不推进 Input 偏移空间，只跳过数据；
            // 但它们仍是 Main item，必须消费局部 usage（否则残留的 Usage(0xE9)
            // 会劫持紧随其后的修饰键范围 `19 E0 29 E7`，见通用解析器同处注释）。
            if ((prefix & 0xFC) == 0x80 || (prefix & 0xFC) == 0x90 ||
                (prefix & 0xFC) == 0xB0) {
                loc_usage_count = 0;
                loc_has_usage_range = false;
            }
            i += sz;
            break;
        }
    }

    // 各 Report ID 的报文长度（含键位段之外的集合，如同一接口上的鼠标/厂商段）
    desc->num_lens = lens_scan(data, len, desc->lens,
                               HID_REPORT_LEN_TABLE, &desc->lens_overflow);

    return desc->num_spans > 0;
}

// 单键边沿检测 + shadow 更新（所有段共用一份 shadow，按 keycode 索引；
// 位图段与数组段、跨 Report ID 的重复键都由此去重）
/* keycode 是否落进 shadow 位图可记的范围。
 *
 * 单独抽成函数（参数是 uint16_t）是为了避开 -Wtype-limits：容量为默认 32 字节时
 * HID_NKRO_KEY_MAX 是 255，直接在调用点拿 uint8_t 的 kc 去比 `<= 255`，
 * GCC 会判定"恒真"并报错（-Werror 下编不过）；参数提升到 uint16_t 后
 * 这个比较对编译器就不再是恒真。 */
static inline bool HIDKIT_HOT(nkro_key_fits)(uint16_t keycode)
{
    return keycode >= HID_NKRO_KEY_MIN && keycode <= HID_NKRO_KEY_MAX;
}

static void HIDKIT_HOT(nkro_key_edge)(int8_t slot, hid_nkro_dev_t *dev, uint16_t keycode, bool down)
{
    // 上限是 shadow 位图容量（HIDKIT_NKRO_BYTES 可调小）：越界键码不记不报
    if (!nkro_key_fits(keycode)) return;

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

bool HIDKIT_HOT(hid_nkro_accepts)(const hid_nkro_dev_t *dev,
                                  const uint8_t *report, uint16_t len)
{
    return hid_nkro_match(dev, report, len) > 0;
}

uint8_t HIDKIT_HOT(hid_nkro_match)(const hid_nkro_dev_t *dev,
                                   const uint8_t *report, uint16_t len)
{
    if (!dev || dev->desc.num_spans == 0) return 0;

    bool has_id = false;
    for (uint8_t s = 0; s < dev->desc.num_spans; s++) {
        if (dev->desc.spans[s].report_id != 0) {
            has_id = true;
            break;
        }
    }

    uint8_t id = 0;
    if (has_id) {
        if (!report || len < 1) return 0;
        bool hit = false;
        for (uint8_t s = 0; s < dev->desc.num_spans; s++) {
            if (dev->desc.spans[s].report_id == report[0]) {
                hit = true;
                break;
            }
        }
        if (!hit) return 0;
        id = report[0];
    } else if (!report || len == 0) {
        return 0;
    }

    /* 与 hid_mouse_match 对称：描述符声明的输入数据体位宽必须被报文装下。
     * 挡掉的是"根本装不下本集合布局"的报文（短报文、厂商私包、别的 Report
     * ID 的截断件）——那类报文按位图硬解出来全是噪声键。 */
    uint16_t const body_len = (uint16_t)(len - (has_id ? 1 : 0));
    const hid_report_len_t *e = lens_find(dev->desc.lens, dev->desc.num_lens, id);
    if (!e) return dev->desc.lens_overflow ? 0 : 1;
    uint16_t const want = (uint16_t)((e->bits + 7u) / 8u);
    if (want != 0 && body_len < want) return 0;
    return (body_len == want) ? 2 : 1;
}

bool HIDKIT_HOT(hid_nkro_uses_report_id)(const hid_nkro_dev_t *dev)
{
    if (!dev) return false;
    for (uint8_t s = 0; s < dev->desc.num_spans; s++) {
        if (dev->desc.spans[s].report_id != 0) return true;
    }
    return false;
}

void HIDKIT_HOT(hid_nkro_dispatch)(int8_t slot, hid_nkro_dev_t *dev, const uint8_t *report, uint16_t len)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev || !report || len == 0 || dev->desc.num_spans == 0) return;

    // 数组段本帧的键码集合（所有数组段合并；8-bit keycode → 256 位位图）
    uint8_t array_now[HIDKIT_NKRO_BYTES];
    bool    has_array = false;

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
            for (uint16_t j = 0; j < sp->count; j++) {
                uint32_t bit = (uint32_t)sp->bit_offset + j;
                if ((bit >> 3) >= body_len) break;   // 段超出报文长度（防越界）
                bool down = (body[bit >> 3] >> (bit & 7)) & 1;
                nkro_key_edge(slot, dev, sp->usage_min + j, down);
            }
        } else {
            // 数组段：先收集本帧键码，稍后按"新旧集合差"统一做边沿
            if (!has_array) {
                memset(array_now, 0, sizeof(array_now));
                has_array = true;
            }
            for (uint16_t j = 0; j < sp->count; j++) {
                // 数组段：第 j 槽的字节值即 keycode（非字节对齐时走通用位域读取）
                hid_field_t f = { .bit_offset = (uint16_t)(sp->bit_offset + 8u * j),
                                  .bit_size = 8, .logical_min = 0 };
                uint8_t kc = (uint8_t)hid_field_read(&f, body, body_len);
                if (nkro_key_fits(kc)) {
                    array_now[kc >> 3] |= (uint8_t)(1u << (kc & 7));
                }
            }
        }
    }

    // 数组段边沿：数组报文只列出"当前按着的键"，松开的键**不会出现**，
    // 所以必须拿上一帧集合做差，不能只对本帧出现的键调 nkro_key_edge ——
    // 旧实现漏了这一步，数组式键盘（6KRO / boot 风格描述符）一松键，
    // shadow 里该键永远按着，上层再也等不到松开。
    //
    // 说明：array_prev 是"本设备所有数组段"共用的一份集合（同一 keycode 同时出现在
    // 位图段与数组段的描述符极罕见，此时数组段的松开可能误释放该键）。多个数组段
    // 分属不同 Report ID 时也共用这份集合，属已知边角限制。
    if (has_array) {
        // 上界用 HIDKIT_NKRO_BYTES（不是 256）：缓冲调小时键码空间跟着变小
        for (uint16_t kc = 0; kc <= HID_NKRO_KEY_MAX; kc++) {
            bool now    = (array_now[kc >> 3] >> (kc & 7)) & 1u;
            bool before = (dev->array_prev[kc >> 3] >> (kc & 7)) & 1u;
            if (now && !before)      nkro_key_edge(slot, dev, kc, true);
            else if (!now && before) nkro_key_edge(slot, dev, kc, false);
        }
        memcpy(dev->array_prev, array_now, sizeof(dev->array_prev));
    }
}

void HIDKIT_HOT(hid_nkro_release_all)(int8_t slot, hid_nkro_dev_t *dev)
{
    // hidkit：入口做槽位守界，越界直接丢弃（等价于原有越界丢弃）
    if (slot < 0 || slot >= (int8_t)HIDKIT_MAX_SLOTS) return;
    if (!dev) return;
    for (uint16_t kc = 0; kc <= HID_NKRO_KEY_MAX; kc++) {
        if (dev->last_keys[kc >> 3] & (1u << (kc & 7))) {
            // hidkit：改用统一出口（键盘段前缀 + HID Usage ID）
            hidkit_emit_key(slot, (uint16_t)(HIDKIT_CODE_KEYBOARD | kc), false);
        }
    }
    memset(dev->last_keys, 0, sizeof(dev->last_keys));
    memset(dev->array_prev, 0, sizeof(dev->array_prev));
}
