/**
 * @file gamepad.h
 * @brief 通用游戏手柄支持 —— 公共类型定义 + API 声明
 *
 * 所有手柄（HID/XInput）最终归一化为 hidkit_gamepad_state_t（见 hidkit.h）。
 * buttons 保留手柄原生位布局，通过 btn_map 查表获知每 bit 含义。
 *
 * 典型调用流程：
 *   mount:   gamepad_hid_mount(slot, vid, pid, desc_report, desc_len)
 *   report:  gamepad_hid_dispatch(slot, report, len, &state)
 *   umount:  gamepad_hid_umount(slot)
 *
 * 外部边沿检测：
 *   uint32_t changed = now.buttons ^ last.buttons;
 *   for (int i = 0; i < 32; i++) {
 *       if (changed & (1u << i)) {
 *           uint16_t btn = now.btn_map[i];        // 查表
 *           bool pressed = now.buttons & (1u << i);
 *           // → hidkit_emit_key(slot, HIDKIT_CODE_GAMEPAD | (btn & 0xFF), pressed)
 *       }
 *   }
 */

#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <stdint.h>
#include <stdbool.h>
#include "hidkit.h"        // hidkit_gamepad_state_t / BTN_* / DPAD_*

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------+
 * 归一化手柄状态（控制器无关）
 *
 * hidkit：本模块不再自定义手柄状态结构，直接用 hidkit.h 里的公共类型
 * （字段完全一致），只保留一个名字别名供本模块内部沿用 gamepad_state_t。
 *
 * buttons 保留手柄原生位布局，通过 btn_map 查表获知每 bit 含义。
 * 按钮类型定义见 hidkit_codes.h: BTN_A..BTN_EXTRA_8
 *--------------------------------------------------------------------*/

typedef hidkit_gamepad_state_t gamepad_state_t;

/*--------------------------------------------------------------------+
 * 公共 API
 *--------------------------------------------------------------------*/

/**
 * @brief 已知手柄的 VID/PID 匹配（纯查表，不碰槽位状态）
 *
 * 用途：hidkit_mount() 需要在**分配槽位之前**判断"这个设备是不是已知手柄"
 * （命中才占槽位）。原来这一步是拿 gamepad_hid_mount(-1, ...) 当探测用的，
 * 但该入口有 slot < 0 守界，于是探测恒失败 —— 所有已知手柄（DS5/Azeron）
 * 都被判成"不认识"，随后落到 NKRO 分支、最终 overall 返回未消费。
 * 探测与挂载拆成两个函数后这类误用不可能再发生。
 *
 * @return true = 是本表内的已知手柄
 */
bool gamepad_hid_match(uint16_t vid, uint16_t pid);

/**
 * @brief 手柄 HID 设备挂载：按 VID/PID 匹配已知手柄，注册解析函数
 *
 * @param slot         本设备占用的槽位（< 0 或越界一律返回 false）
 * @param vid          USB Vendor ID
 * @param pid          USB Product ID
 * @param desc_report  HID Report Descriptor 原始数据（当前未使用，预留给描述符解析）
 * @param desc_len     描述符长度（字节）
 * @return true 匹配成功，false 未知手柄
 */
bool gamepad_hid_mount(int8_t slot, uint16_t vid, uint16_t pid,
                       const uint8_t *desc_report, uint16_t desc_len);

/**
 * @brief 手柄 HID 设备卸载
 */
void gamepad_hid_umount(int8_t slot);

/**
 * @brief 解析 HID 报告，输出归一化手柄状态（无状态，每帧独立）
 *
 * @param slot         本设备占用的槽位
 * @param report       原始 HID 报告数据
 * @param len          报告长度（字节）
 * @param out_state    输出：归一化状态（含 btn_map 指针）
 * @return true 解析成功，out_state 有效
 */
bool gamepad_hid_dispatch(int8_t slot,
                          const uint8_t *report, uint16_t len,
                          gamepad_state_t *out_state);

/**
 * @brief 查询槽位是否有已解析的活跃手柄
 */
bool gamepad_is_active(int8_t slot);


/**
 * @brief 查询槽位是否有已解析的活跃手柄，且 VID/PID 匹配
 */
bool valid_gp_pid_vid(int8_t slot, uint16_t vid, uint16_t pid);


#ifdef __cplusplus
}
#endif

#endif // GAMEPAD_H
