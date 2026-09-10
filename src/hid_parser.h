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
} hid_mouse_desc_t;

// 鼠标设备句柄 = 描述符 + 边沿检测状态
typedef struct {
    hid_mouse_desc_t desc;
    uint8_t          last_buttons;   // 上一帧按键位掩码
    bool             initialized;    // 是否已收到过报告（首帧不触发边沿）
} hid_mouse_dev_t;

// ---- NKRO 键盘 ----

// hidkit：NKRO_MAX_SPANS / NKRO_MAX_ARRAY 是本模块私有的容量常量，无对应的
// HIDKIT_* 配置宏，保留原值；shadow 位图字节数改用 HIDKIT_NKRO_BYTES（可 -D 覆盖）
#define HID_NKRO_MAX_SPANS    8    // 单接口最多键位段（主位图/修饰键/小键盘常分 2~3 段）
#define HID_NKRO_MAX_ARRAY    32   // 数组段最多槽数（6KRO=6；Win8 全键 Rollover 可达全键）
#define HID_NKRO_KEY_MIN      0x04 // 0x00-0x03 为 ErrorRollOver 等保留键，不产生事件

// NKRO 键位段：一段连续 keycode 的位图（或 keycode 数组）
typedef struct {
    uint16_t bit_offset;   // 报告内位偏移（不含 Report ID 字节）
    uint8_t  usage_min;    // 位图段起始 keycode（数组段未用）
    uint8_t  count;        // 位图段：位数；数组段：槽数
    uint8_t  bit_size;     // 1 = 位图段；8 = 数组段（字节值即 keycode）
    uint8_t  report_id;    // 该段所属 Report ID（0 = 无 Report ID）
} hid_nkro_span_t;

// NKRO 描述符解析结果（不逐键展开——104 键展开既超 HID_PARSE_MAX_FIELDS 又浪费 RAM，
// 只记录位段 + usage_min，dispatch 时按位索引推算 keycode）
typedef struct {
    hid_nkro_span_t spans[HID_NKRO_MAX_SPANS];
    uint8_t num_spans;
} hid_nkro_desc_t;

// NKRO 设备句柄 = 描述符 + shadow 位图（按 keycode 索引的按下状态，用于边沿检测与拔出补发）
typedef struct {
    hid_nkro_desc_t desc;
    uint8_t last_keys[HIDKIT_NKRO_BYTES];   // hidkit：容量改由 HIDKIT_NKRO_BYTES 配置
} hid_nkro_dev_t;

/*--------------------------------------------------------------------+
 * API
 *--------------------------------------------------------------------*/

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
 * @brief 释放 NKRO 键盘 shadow 位图中仍按着的所有键（拔出时补发松开）
 *
 * @param slot 本设备占用的槽位（发起事件时原样带回）
 * @param dev  NKRO 设备句柄（含描述符 + shadow 位图）
 */
void hid_nkro_release_all(int8_t slot, hid_nkro_dev_t *dev);

#endif // HID_PARSER_H
