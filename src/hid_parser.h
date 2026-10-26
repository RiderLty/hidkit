#ifndef HID_PARSER_H
#define HID_PARSER_H

/**
 * @file hid_parser.h
 * @brief HID 报告描述符解析器
 *
 * 解析 USB HID Report Descriptor，提取字段的偏移/位宽/用法，
 * 并提供按字段元数据解释原始报告的能力。不依赖任何 USB 栈，可在主机上跑样本测试。
 *
 * 三个解析路径：
 *   - hid_mouse_parse/hid_mouse_dispatch: 鼠标描述符解析 + dispatch
 *   - hid_nkro_parse/hid_nkro_dispatch:   NKRO 键盘描述符解析 + dispatch（span 式）
 *   - gamepad.c:                          手柄描述符解析 + dispatch（通过 hid_parse_report_fields）
 *   - hid_dispatch_mouse/hid_dispatch_keyboard: 硬编码固定格式（boot 协议）
 */

#include <stdint.h>
#include <stdbool.h>

#include "hidkit_config.h"   // HIDKIT_* 容量宏

/*--------------------------------------------------------------------+
 * 配置常量
 *--------------------------------------------------------------------*/

// hidkit：下面几个是本模块私有的容量常量，无对应的 HIDKIT_* 配置宏，保留原值
#define HID_MOUSE_MAX_FIELDS   24   // 鼠标描述符最多字段数（展开后）
#define HID_MOUSE_MAX_USAGES   8    // 单个 Input item 最多 usage 数
#define HID_MOUSE_MAX_BTNS     8    // 鼠标按键数上限（= uint8_t 位掩码宽度，与 dispatch 的掩码一致）
#define HID_PARSE_MAX_FIELDS   64   // 通用描述符解析器最多字段数

/*--------------------------------------------------------------------+
 * HID Usage 定义
 *--------------------------------------------------------------------*/

// Usage Page
#define HID_USAGE_PAGE_GENERIC_DESKTOP  0x01
#define HID_USAGE_PAGE_KEYBOARD         0x07
#define HID_USAGE_PAGE_BUTTON           0x09

// Generic Desktop Usage ID
#define HID_USAGE_X_AXIS      0x30
#define HID_USAGE_Y_AXIS      0x31
#define HID_USAGE_Z_AXIS      0x32
#define HID_USAGE_RX_AXIS     0x33
#define HID_USAGE_RY_AXIS     0x34
#define HID_USAGE_RZ_AXIS     0x35
#define HID_USAGE_SLIDER      0x36
#define HID_USAGE_DIAL        0x37
#define HID_USAGE_WHEEL       0x38
#define HID_USAGE_HAT_SWITCH  0x39
#define HID_USAGE_DPAD_UP     0x90
#define HID_USAGE_DPAD_DOWN   0x91
#define HID_USAGE_DPAD_LEFT   0x92
#define HID_USAGE_DPAD_RIGHT  0x93

// Generic Desktop top-level usage
#define HID_USAGE_DESKTOP_POINTER  0x01
#define HID_USAGE_DESKTOP_MOUSE    0x02
#define HID_USAGE_DESKTOP_JOYSTICK 0x04
#define HID_USAGE_DESKTOP_GAMEPAD  0x05
#define HID_USAGE_DESKTOP_KEYBOARD 0x06
#define HID_USAGE_DESKTOP_MULTI_AXIS 0x08

// Collection types (0xA1 的参数)
#define HID_COLLECTION_PHYSICAL    0x00
#define HID_COLLECTION_APPLICATION 0x01

// Input flags (0x81 的参数)
#define HID_INPUT_CONSTANT  0x01
#define HID_INPUT_VARIABLE  0x02
#define HID_INPUT_RELATIVE  0x04

/*--------------------------------------------------------------------+
 * 数据结构
 *--------------------------------------------------------------------*/

/* 每个 Report ID 的输入报文数据体位宽（不含 Report ID 字节）。
 *
 * 描述符就是报文长度的**权威来源**：Report Size × Report Count 累加出多少位，
 * 设备在固定长度的中断端点上就发多少字节。Linux hid-core 用同一判据
 * （`report->size` → `hid_compute_report_size()`，不含 ID 字节）在
 * `hid_report_raw_event()` 里拒收短报文，本表就是它的移植。
 *
 * 为什么必须校验整份报文长度：字段级的越界检查只保证"这个字段读得到"，
 * 读得到不等于读得对。真机上 11 字节鼠标接口收到 2 字节杂报文（BE EF）时，
 * X 轴字段（bit 8..23）恰好落在其中 → 解出 dx = -4162 的假位移，
 * 上层表现为"没动鼠标但光标一直飞"。 */
typedef struct {
    uint8_t  id;         // Report ID（0 = 无 ID 前缀）
    uint16_t bits;       // 该 Report ID 的输入数据体总位数（Output/Feature 不计）
    uint16_t bits_kb;    // 其中属于最外层键盘集合的位数（0 = 该 ID 没有键盘集合）
    uint16_t bits_mouse; // 其中属于最外层鼠标集合的位数（0 = 该 ID 没有鼠标集合）
} hid_report_len_t;

#define HID_REPORT_LEN_TABLE 8   // 一张表最多记 8 个 Report ID

/* 超出表容量的 Report ID 会被标成"长度未声明"（lens_overflow）。
 * 这时匹配函数**拒绝**该 ID 的报文（而不是无条件接受）—— 接受等于把
 * "短报文造假位移"重新打开；拒绝只是把这份报文交回宿主。 */

// 展开后的单个 HID 字段（一个 usage → 一个字段）
typedef struct {
    uint16_t usage_page;    // Usage Page（如 0x01 = Generic Desktop）
    uint16_t usage_id;      // Usage ID（如 0x30 = X 轴）
    uint16_t bit_offset;    // 在报告中的位偏移（不含 Report ID 字节）
    uint8_t  bit_size;      // 位宽（1~32）
    uint8_t  report_id;     // 本字段所属的 Report ID（0 = 无 Report ID 前缀）
                            // hidkit：多 Report ID 描述符按**字段各自**记录（原来全描述符只留最后一个）
    int32_t  logical_min;   // 逻辑最小值
    int32_t  logical_max;   // 逻辑最大值
    bool     is_relative : 1;  // 相对值（Relative）
    bool     is_constant : 1;  // 常量（Constant，通常为 padding）
} hid_field_t;

// 鼠标描述符解析结果
typedef struct {
    hid_field_t fields[HID_MOUSE_MAX_FIELDS];
    uint8_t     num_fields;         // 实际字段数
    uint8_t     report_id;          // 描述符主 Report ID = 首个出现的非零 Report ID
                                    // （0 = 整份描述符都没有 Report ID）
                                    // 多 ID 描述符里各字段自己的 ID 见 hid_field_t.report_id

    // 预查索引（0xFF = 不存在），指向 fields[] 数组下标
    uint8_t     idx_x;              // X 轴字段
    uint8_t     idx_y;              // Y 轴字段
    uint8_t     idx_wheel;          // 滚轮字段
    // 按键字段下标表（按下标顺序）。多 Report ID 描述符里按键可能不成组，
    // 因此不像原来那样只记"起始字段 + 个数"。
    uint8_t     btn_idx[HID_MOUSE_MAX_BTNS];
    uint8_t     button_count;       // 按键个数（≤ HID_MOUSE_MAX_BTNS）

    // 各 Report ID 的报文数据体长度（整份描述符累计，含被 mouse_field_keep
    // 滤掉的集合）——接受报文前用它校验"这份报文根本不可能是我的"
    hid_report_len_t lens[HID_REPORT_LEN_TABLE];
    uint8_t     num_lens;
    bool        lens_overflow;      // 描述符的 Report ID 多于表容量（见 hid_desc_report_bits）
} hid_mouse_desc_t;

// 鼠标设备句柄 = 描述符 + 边沿检测状态
typedef struct {
    hid_mouse_desc_t desc;
    uint8_t          last_buttons;   // 上一帧按键位掩码（初值 0 = 全部抬起）
} hid_mouse_dev_t;

// ---- NKRO 键盘 ----

// hidkit：NKRO_MAX_SPANS / NKRO_MAX_ARRAY 是本模块私有的容量常量，无对应的
// HIDKIT_* 配置宏，保留原值；shadow 位图字节数改用 HIDKIT_NKRO_BYTES（可 -D 覆盖）
#define HID_NKRO_MAX_SPANS    8    // 单接口最多键位段（主位图/修饰键/小键盘常分 2~3 段）
#define HID_NKRO_MAX_ARRAY    32   // 数组段最多槽数（6KRO=6；Win8 全键 Rollover 可达全键）
#define HID_NKRO_KEY_MIN      0x04 // 0x00-0x03 为 ErrorRollOver 等保留键，不产生事件

// shadow 位图能记的最高 keycode（HIDKIT_NKRO_BYTES*8-1）：调小缓冲时超出它的键
// 既不记也不发事件 —— 宁可少键，不能越界写
#define HID_NKRO_KEY_MAX      (HIDKIT_NKRO_BYTES * 8 - 1)

// NKRO 键位段：一段连续 keycode 的位图（或 keycode 数组）
typedef struct {
    uint16_t bit_offset;   // 报告内位偏移（不含 Report ID 字节）
    uint8_t  usage_min;    // 位图段起始 keycode（数组段未用）
    uint16_t count;        // 位图段：位数；数组段：槽数（16 位，兼容 256 位全键位图）
    uint8_t  bit_size;     // 1 = 位图段；8 = 数组段（字节值即 keycode）
    uint8_t  report_id;    // 该段所属 Report ID（0 = 无 Report ID）
} hid_nkro_span_t;

// NKRO 描述符解析结果（不逐键展开——104 键展开既超 HID_PARSE_MAX_FIELDS 又浪费 RAM，
// 只记录位段 + usage_min，dispatch 时按位索引推算 keycode）
typedef struct {
    hid_nkro_span_t spans[HID_NKRO_MAX_SPANS];
    uint8_t num_spans;

    // 各 Report ID 的报文数据体长度（整份描述符累计，含键位段之外的集合）
    hid_report_len_t lens[HID_REPORT_LEN_TABLE];
    uint8_t num_lens;
    bool    lens_overflow;          // 描述符的 Report ID 多于表容量
} hid_nkro_desc_t;

// NKRO 设备句柄 = 描述符 + shadow 位图（按 keycode 索引的按下状态，用于边沿检测与拔出补发）
typedef struct {
    hid_nkro_desc_t desc;
    uint8_t last_keys[HIDKIT_NKRO_BYTES];   // hidkit：容量改由 HIDKIT_NKRO_BYTES 配置
    uint8_t array_prev[HIDKIT_NKRO_BYTES];  // 数组段上一帧的键码集合（松开要靠新旧集合差检测）
} hid_nkro_dev_t;

/*--------------------------------------------------------------------+
 * API
 *--------------------------------------------------------------------*/

/**
 * @brief 字段保留回调（可选）：返回 false 的字段只推进位偏移、不占用 fields[] 槽位
 *
 * 用途：多集合接口（一个描述符里既有厂商集合又有鼠标集合）里，前面的集合可能
 * 把字段表占满；用它能"跳过不关心的集合"，让后面的鼠标集合仍能被收录。
 */
typedef bool (*hid_field_keep_fn)(uint16_t usage_page, uint16_t usage_id);

/**
 * @brief 通用 HID 报告描述符字段解析器（共享）
 *
 * 遍历 HID Report Descriptor，展开所有 Input item 中的字段到 hid_field_t[] 数组。
 * 不关心 usage 含义，只记录字段元数据（偏移/位宽/逻辑范围/usage 等）。
 * 调用者自行对返回的 fields[] 做后处理索引。
 *
 * @param fields      输出：展开后的字段数组（每个字段自带所属 Report ID）
 * @param max_fields  数组容量
 * @param report_id   输出：描述符主 Report ID = 首个非零 Report ID（0 = 无 Report ID 字节）
 * @param data        输入的 HID Report Descriptor 二进制数据
 * @param len         描述符长度（字节）
 * @return 字段数（0 表示解析失败或无双字段）
 */
uint8_t hid_parse_report_fields(hid_field_t *fields, uint8_t max_fields,
                                 uint8_t *report_id,
                                 const uint8_t *data, uint16_t len);

/**
 * @brief 带字段过滤的通用描述符解析（keep == NULL 等价于 hid_parse_report_fields）
 *
 * 无论字段是否被保留，位偏移都按 Report Count/Size 推进 —— 被过滤掉的集合
 * 不会让后续字段偏移错位。
 */
uint8_t hid_parse_report_fields_filtered(hid_field_t *fields, uint8_t max_fields,
                                         uint8_t *report_id,
                                         const uint8_t *data, uint16_t len,
                                         hid_field_keep_fn keep);


/**
 * @brief 从描述符统计每个 Report ID 的输入报文数据体位宽
 *
 * 只累加 Input item（Output/Feature 不占输入报文长度），Report ID 另记
 * （不计入数据体位宽）。每一位宽同时按最外层 Application Collection 归类
 * （键盘 / 鼠标 / 其它），供无 Report ID 的键鼠共接口设备按长度分流。
 *
 * @param data    描述符
 * @param len     描述符长度
 * @param out     输出表
 * @param max_out 表容量（≤ HID_REPORT_LEN_TABLE）
 * @return 写入的条目数（0 = 没有 Input item）
 */
uint8_t hid_desc_report_bits(const uint8_t *data, uint16_t len,
                             hid_report_len_t *out, uint8_t max_out);

/**
 * @brief 描述符是否声明了鼠标（顶层 Application Collection 为 Mouse/Pointer）
 *
 * 用于复合设备判定：「键盘 + 鼠标」同接口时，接口的 bInterfaceProtocol 往往
 * 只写 Keyboard（或 0），不能据此放弃鼠标集合。只看 X/Y/Button 字段又不够——
 * 手柄描述符里同样有这些字段，故以顶层集合 usage（Generic Desktop 0x02/0x01）
 * 为判据。
 */
bool hid_desc_has_mouse_collection(const uint8_t *data, uint16_t len);

/**
 * @brief 解析鼠标 HID 报告描述符
 *
 * @param desc  输出：解析结果
 * @param data  输入的 HID Report Descriptor 二进制数据
 * @param len   描述符长度（字节）
 * @return true 解析成功，false 失败（无效参数或字段溢出）
 */
bool hid_mouse_parse(hid_mouse_desc_t *desc, const uint8_t *data, uint16_t len);

/**
 * @brief 从报告数据中按字段元数据提取值
 *
 * 处理跨字节位域对齐和符号扩展（当 logical_min < 0 时）。
 *
 * @param f          字段元数据
 * @param report     原始 HID 报告数据
 * @param report_len 报告长度（字节）
 * @return 提取的整数值（已做符号扩展）
 */
int32_t hid_field_read(const hid_field_t *f,
                       const uint8_t *report, uint16_t report_len);

/**
 * @brief 独立鼠标报告 dispatch
 *
 * 按描述符从 report 动态提取按键/X/Y/Wheel 值，
 * 边沿检测后经统一出口发事件（hidkit_emit_key / hidkit_emit_mouse_abs）。
 *
 * 与 hid_dispatch_mouse() 最终走到同一个出口，但报告解析方式不同。
 *
 * @param slot    本设备占用的槽位（发起事件时原样带回）
 * @param dev     鼠标设备句柄（含描述符 + 边沿检测状态）
 * @param report  原始 HID 报告数据
 * @param len     报告长度（字节）
 */
void hid_mouse_dispatch(int8_t slot, hid_mouse_dev_t *dev,
                        const uint8_t *report, uint16_t len);

/**
 * @brief 这份报文是否属于本鼠标描述符（按字段的 Report ID + 描述符声明的长度判定）
 *
 * 多集合/多 Report ID 接口上，host 用它在多个解析器之间路由：
 *   - Report ID：描述符里的相关字段都没有 Report ID（0）时整份报文即数据体；
 *     否则要求 len >= 1 且 report[0] 命中某个 X/Y/Wheel/按键字段的 Report ID
 *   - 长度：描述符声明的**整份报文**位宽必须被报文装下（不是只算鼠标字段：
 *     设备永远按描述符声明的总量发包，按字段位宽比会把真正的杂报文放进来）。
 *     <b>短报文一律拒绝</b> —— 字段级越界检查只保证"读得到"，2 字节杂报文
 *     照样能把 11 字节鼠标的 X 轴解出一个假位移（真机 BE EF → dx=-4162）。
 *   - 描述符的 Report ID 多于长度表容量时（`lens_overflow`），表外的 ID 一律
 *     不接受 —— 交宿主，而不是猜。
 *
 * @return true 表示这份报文应由本解析器处理
 */
bool hid_mouse_accepts(const hid_mouse_dev_t *dev,
                       const uint8_t *report, uint16_t len);

/**
 * @brief 与 hid_mouse_accepts 相同，但返回匹配强度，供多解析器之间择优
 *
 * @return 0 = 不是我的报文；1 = 长度够；2 = 长度与描述符精确吻合
 *
 * 无 Report ID 的"键盘 + 鼠标"同接口设备（廉价 2.4G 接收器）里，键盘与鼠标
 * 报文都命不中 Report ID，只能靠长度分流：此时长度吻合（2）胜过仅仅够长（1）。
 */
uint8_t hid_mouse_match(const hid_mouse_dev_t *dev,
                        const uint8_t *report, uint16_t len);

/**
 * @brief 本鼠标描述符的相关字段（X/Y/Wheel/按键）是否带显式 Report ID
 *
 * 路由用：同一接口同时有键盘与鼠标能力时，两份解析器是否处理同一份报文交给
 * hidkit_report() 按长度判定（`hid_mouse_match` / `hid_nkro_match` 的匹配强度）。
 * 本函数只回答"字段是否带显式 Report ID"，供需要区分布局的调用方使用。
 */
bool hid_mouse_uses_report_id(const hid_mouse_dev_t *dev);

/**
 * @brief 释放鼠标 shadow 里仍按着的按键（拔出/卸载时补发松开）
 *
 * hid_mouse_dispatch 用的是 hid_mouse_dev_t.last_buttons，与 boot 鼠标的
 * hid_dispatch_reset 不是同一份状态 —— 卸载时必须显式调用，否则按住鼠标键
 * 拔掉设备，上层永远等不到松开。
 */
void hid_mouse_release_all(int8_t slot, hid_mouse_dev_t *dev);


/**
 * @brief 解析 NKRO 键盘 HID 报告描述符
 *
 * 识别 Usage Page 0x07（Keyboard/Keypad）上的键位段：
 *   - Report Size 1 的位图段（NKRO 报文本体，含 0xE0-0xE7 修饰键段）
 *   - Report Size 8 的数组段（6KRO / Win8 全键 Rollover 报文）
 * 其他 Usage Page（Consumer/系统控制等）与 Output(LED)/Feature 项跳过。
 * 同接口多 Report ID 的描述符按段各自记录 ID。
 *
 * @param desc 输出：解析结果
 * @param data 输入的 HID Report Descriptor 二进制数据
 * @param len  描述符长度（字节）
 * @return true 识别到至少一个键位段，false 非 NKRO 键盘描述符
 */
bool hid_nkro_parse(hid_nkro_desc_t *desc, const uint8_t *data, uint16_t len);

/**
 * @brief NKRO 键盘报告 dispatch
 *
 * 按描述符键位段从 report 提取按键状态，与 shadow 位图做边沿检测后
 * 经统一出口发按键事件（keycode 与键盘段前缀拼成 code；修饰键 0xE0-0xE7
 * 与普通键同一入口，与 hid_dispatch_keyboard 的映射语义一致）。
 *
 * @param slot   本设备占用的槽位（发起事件时原样带回）
 * @param dev    NKRO 设备句柄（含描述符 + shadow 位图）
 * @param report 原始 HID 报告数据（含 Report ID 字节，如有）
 * @param len    报告长度（字节）
 */
void hid_nkro_dispatch(int8_t slot, hid_nkro_dev_t *dev, const uint8_t *report, uint16_t len);

/**
 * @brief 这份报文是否属于本 NKRO 描述符（按键位段的 Report ID + 声明长度判定）
 *
 * 语义与 hid_mouse_accepts 对称：Report ID 命中（或描述符无 ID）且报文装得下
 * 描述符声明的**整份报文**位宽。短报文一律拒绝；表外 Report ID 同样拒绝。
 *
 * @return true 表示这份报文应由本解析器处理
 */
bool hid_nkro_accepts(const hid_nkro_dev_t *dev,
                      const uint8_t *report, uint16_t len);

/**
 * @brief 与 hid_nkro_accepts 相同，但返回匹配强度（0/1/2）
 *
 * 语义与 hid_mouse_match 对称；无 Report ID 的键鼠共接口设备靠它把鼠标报文
 * 从键盘解析器里分流出去。
 */
uint8_t hid_nkro_match(const hid_nkro_dev_t *dev,
                       const uint8_t *report, uint16_t len);

/**
 * @brief 本 NKRO 描述符的键位段是否带显式 Report ID（语义与
 *        hid_mouse_uses_report_id 对称）
 */
bool hid_nkro_uses_report_id(const hid_nkro_dev_t *dev);

/**
 * @brief 释放 NKRO 键盘 shadow 位图中仍按着的所有键（拔出时补发松开）
 *
 * @param slot 本设备占用的槽位（发起事件时原样带回）
 * @param dev  NKRO 设备句柄（含描述符 + shadow 位图）
 */
void hid_nkro_release_all(int8_t slot, hid_nkro_dev_t *dev);

#endif // HID_PARSER_H
