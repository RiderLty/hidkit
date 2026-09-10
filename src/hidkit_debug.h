#ifndef HIDKIT_DEBUG_H
#define HIDKIT_DEBUG_H

/*
 * 库内诊断输出（默认关闭，编译期裁剪）。
 *
 * 定位：只打"库自己怎么想的"这类自证信息，全部在**冷路径**（挂载/卸载/认领判定/
 * 描述符解析失败/槽位挤出/报文被丢弃的第一次），**绝不**放进每报文的解析里。
 * 逐报文的可观测性由使用方的 hidkit_input_* 出口函数承担 —— 那里你可以按需打印。
 *
 * 输出方式是**弱符号函数**，库自己**不引 stdio**：
 *   - 不定义：库内是空实现，`HIDKIT_DEBUG=0` 时连调用点都被 `#if` 裁掉；
 *   - 要输出：在自己的工程里实现 `hidkit_debug_printf`，接到你的日志通道（UART /
 *     SEGGER RTT / 串口队列…）即可，例如
 *
 *       void hidkit_debug_printf(const char *fmt, ...) { ... vprintf 到你的日志 ... }
 *
 * 注意：这是**纯诊断**，不参与数据流；改数据用 hidkit_hook_*，接输入管线用
 * hidkit_input_*。
 */

#include <stdarg.h>
#include "hidkit_config.h"

/* 诊断输出出口（弱符号，默认空实现） */
void hidkit_debug_printf(const char *fmt, ...);

#if HIDKIT_DEBUG
#define HIDKIT_LOG(...) hidkit_debug_printf(__VA_ARGS__)
#else
#define HIDKIT_LOG(...) ((void)0)
#endif

#endif /* HIDKIT_DEBUG_H */
