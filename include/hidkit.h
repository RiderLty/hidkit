#ifndef HIDKIT_H
#define HIDKIT_H

/*
 * hidkit —— 可移植的 HID / XInput 设备解析库
 *
 * 职责边界：只做"把 USB 栈送来的描述符与报文解析成标准事件，并跑回调"。
 * 不碰具体 USB 栈（TinyUSB / CherryUSB / ESP-IDF …）、不碰 OS、不分配堆、
 * 不依赖任何平台头文件。宿主只需提供下面 hidkit_dev_info_t 里的"基本信息"。
 *
 * 内存模型：库内全部 static，槽位数量由 hidkit_config.h 的宏定死（无上下文对象）。
 *
 * 典型接线（以 TinyUSB 为例）：
 *   hidkit_init(&cb);                                    // 一次
 *   tuh_hid_mount_cb:     hidkit_mount(&info);            // 返回 <0 = 本库不认，宿主自行处理
 *   tuh_hid_report_received_cb: hidkit_report(slot, buf, len);
 *   tuh_hid_umount_cb:    hidkit_umount(slot);
 *   XInput 适配器（独立仓库）：hidkit_xinput_report(slot, &pad);
 */

#include <stdint.h>
#include <stdbool.h>

#include "hidkit_config.h"
#include "hidkit_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * 设备信息：宿主要提供的最小契约
 *--------------------------------------------------------------------*/

/* proto 取值（与 TinyUSB 的 hid_interface_protocol_enum_t 语义一致，
 * 但不依赖其头文件；未知/未声明用 0） */
#define HIDKIT_PROTO_NONE     0
#define HIDKIT_PROTO_KEYBOARD 1
#define HIDKIT_PROTO_MOUSE    2
/* 适配器已经确认这是手柄（例如 XInput：没有 HID 描述符，故不能靠描述符判定）。
 * 填这个 proto 调 hidkit_mount() 即认领一个手柄槽位，之后用
 * hidkit_xinput_report() 或 hidkit_report() 送报文。 */
#define HIDKIT_PROTO_GAMEPAD  3

typedef struct {
    uint16_t vid;
    uint16_t pid;
    uint8_t  dev_addr;      /* 宿主栈里的设备标识，本库原样回传，不解释 */
    uint8_t  itf;           /* HID 接口号（固定布局设备填 0） */
    uint8_t  proto;         /* HIDKIT_PROTO_* */
    const uint8_t *report_desc;   /* HID 报告描述符；固定布局设备可为 NULL */
    uint16_t report_desc_len;
} hidkit_dev_info_t;

/*--------------------------------------------------------------------+
 * 出口：弱符号函数，**在自己的工程里定义同名函数即可覆盖**
 *
 *   void hidkit_input_key(int8_t slot, uint16_t code, bool pressed) {
 *       if (HIDKIT_CODE_SEG(code) == HIDKIT_CODE_KEYBOARD) { ... }
 *   }
 *
 * 库里给的是空实现（弱符号），不定义也不会链接失败 —— 与 hidkit_hook_* 同一套机制。
 * 这样没有回调注册表、没有函数指针，代码更小、无间接调用（实测在 Cortex-M 上也更好
 * 配合 `__not_in_flash_func` 之类的标注）。
 *--------------------------------------------------------------------*/

/* 键盘 / 鼠标按键 / 手柄按键统一走这里，用 HIDKIT_CODE_* 段前缀区分类型。
 * 仅在状态变化时回调（边沿由库内检测）。 */
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed);

/* 鼠标位移与滚轮（仅鼠标；不做横向滚轮）。仅在非零时回调。 */
void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel);

/* 手柄绝对状态：每份解析成功的报文都会回调（不做去重 —— 手柄报文本身
 * 就是当前绝对状态）。 */
void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt);

/* 可选诊断：设备被丢弃时（槽位耗尽且策略为 DROP_NEW）通知一次。 */
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid);

/*--------------------------------------------------------------------+
 * 手柄布局：设备原生位 + 位→BTN_* 查表（HID 手柄与 XInput 共用同一结构）
 *--------------------------------------------------------------------*/

typedef struct {
    int32_t  ls_x, ls_y, rs_x, rs_y;  /* -32767..32767 */
    int32_t  lt, rt;                  /* 0..32767 */
    uint32_t buttons;                 /* 设备原生位布局 */
    const uint16_t *btn_map;          /* 第 i 位 → BTN_* / DPAD_* */
    uint8_t  btn_count;
} hidkit_gamepad_state_t;

/* XInput 归一化入口：适配器把厂商报文解析成这个结构后交给本库。
 * 字段与 TinyUSB xinput_host 的 xinput_gamepad 一致，避免适配器二次转换。 */
typedef struct {
    uint16_t buttons;       /* XINPUT_GAMEPAD_* 位 */
    uint8_t  left_trigger;  /* 0..255 */
    uint8_t  right_trigger; /* 0..255 */
    int16_t  lx, ly, rx, ry;
} hidkit_xinput_pad_t;

/*--------------------------------------------------------------------+
 * 入口
 *--------------------------------------------------------------------*/

/* 初始化：清空所有槽位与解析状态（可重复调用）。出口函数是弱符号，无需注册。 */
void hidkit_init(void);

/*
 * 设备挂载：按 proto / 报告描述符 / VID:PID 表决定是否接管。
 * 返回 >= 0：本库已接管，后续用该 slot 调用 hidkit_report / hidkit_umount。
 * 返回 HIDKIT_UNHANDLED：本库不认识该设备，宿主可自行处理（扩展点）。
 * 返回 HIDKIT_ERR_NO_SLOT：认识但槽位耗尽（策略为 DROP_NEW 时）。
 */
int8_t hidkit_mount(const hidkit_dev_info_t *dev);

/*
 * 收到报文：解析并触发回调。
 * 返回 true = 已消费；false = 未消费（该 slot 不属于本库，或该设备类型未启用）。
 * 传入 len == 0 直接返回 false。
 */
bool hidkit_report(int8_t slot, const uint8_t *buf, uint16_t len);

/* 设备卸载：补发"全部抬起"后释放槽位。返回 true = 该槽位原本由本库持有。 */
bool hidkit_umount(int8_t slot);

/* XInput：适配器调用（core 只接受归一化 pad，出口与 HID 手柄完全同路）。 */
bool hidkit_xinput_report(int8_t slot, const hidkit_xinput_pad_t *pad);

/* 查询：该槽位是否仍由本库持有 / 是否为手柄 */
bool hidkit_is_active(int8_t slot);
bool hidkit_is_gamepad(int8_t slot);

/* 兼容查询：当前是否有键盘/鼠标在线（0=无 1=键盘 2=鼠标；键鼠同在报鼠标）。 */
uint8_t hidkit_device_type(void);

/* 未接管返回值 */
#define HIDKIT_UNHANDLED   (-1)
#define HIDKIT_ERR_NO_SLOT (-2)

#ifdef __cplusplus
}
#endif

#endif /* HIDKIT_H */
