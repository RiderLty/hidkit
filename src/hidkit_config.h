#ifndef HIDKIT_CONFIG_H
#define HIDKIT_CONFIG_H

/*
 * hidkit 容量与行为配置。
 *
 * 全部用宏定义，可用 -D 覆盖；也可以定义 HIDKIT_USER_CONFIG 指向自己的配置头，
 * 在编译前 include 进来（适合把配置放进项目统一的 board_config.h）。
 *
 * 内存策略：库内全部是 static 数组，没有堆分配，也没有上下文对象 ——
 * 槽位数量由下面这几个宏一次性定死。单片机（Pico/ESP32/STM32）直接照抄即可。
 */

#ifdef HIDKIT_USER_CONFIG
#include HIDKIT_USER_CONFIG
#endif

/* ---- 同时支持的设备数（每个槽位独立保存边沿检测状态）---- */
#ifndef HIDKIT_MAX_KEYBOARDS
#define HIDKIT_MAX_KEYBOARDS 2
#endif

#ifndef HIDKIT_MAX_MICE
#define HIDKIT_MAX_MICE 1
#endif

#ifndef HIDKIT_MAX_GAMEPADS
#define HIDKIT_MAX_GAMEPADS 2
#endif

/* 槽位总数（内部用：键盘 + 鼠标 + 手柄，不做动态分配） */
#ifndef HIDKIT_MAX_SLOTS
#define HIDKIT_MAX_SLOTS \
    (HIDKIT_MAX_KEYBOARDS + HIDKIT_MAX_MICE + HIDKIT_MAX_GAMEPADS)
#endif

/* ---- 解析容量 ---- */

/* NKRO 位图缓冲字节数：32 = 最多 256 个键位（覆盖 HID Usage 0x04..0xFF） */
#ifndef HIDKIT_NKRO_BYTES
#define HIDKIT_NKRO_BYTES 32
#endif

/* 非 NKRO（boot 协议）键盘同时按下的普通键上限。
 * 典型 boot 键盘报文是 8 字节 = 2 个修饰键字节 + 6 个键码，
 * 但部分设备会给出更多键码槽位；这里给出可调上限，超出部分被截断。 */
#ifndef HIDKIT_MAX_KEYS_PER_KB
#define HIDKIT_MAX_KEYS_PER_KB 12
#endif

/* boot 键盘报文里的修饰键字节数（HID 规范为 1，部分厂商给 2） */
#ifndef HIDKIT_MAX_MODS
#define HIDKIT_MAX_MODS 2
#endif

/* 鼠标按键上限（boot 鼠标为 3~5 个；超出部分不输出） */
#ifndef HIDKIT_MAX_MOUSE_BUTTONS
#define HIDKIT_MAX_MOUSE_BUTTONS 5
#endif

/* ---- 槽位耗尽策略 ---- */
#define HIDKIT_OVERFLOW_DROP_NEW    0  /* 超出容量的设备不解析（回调 dropped 告知） */
#define HIDKIT_OVERFLOW_EVICT_IDLE  1  /* 挤掉最久没有报文的槽位（并补发"全部抬起"） */
#ifndef HIDKIT_OVERFLOW
#define HIDKIT_OVERFLOW HIDKIT_OVERFLOW_EVICT_IDLE
#endif

/* ---- 特性开关（按需裁剪代码体积）---- */
/* 手柄支持（含 HID 手柄的布局表与 XInput 归一化入口） */
#ifndef HIDKIT_ENABLE_GAMEPAD
#define HIDKIT_ENABLE_GAMEPAD 1
#endif

/* 设备专属适配器：Azeron 这类"轴走手柄通道 + 键盘直通"的双通道设备。
 * 关掉后这类设备会走 hidkit_mount() 的"未消费"返回，由用户在外部处理。 */
#ifndef HIDKIT_ENABLE_ADAPTER_AZERON
#define HIDKIT_ENABLE_ADAPTER_AZERON 1
#endif

/* 库内诊断输出（详见 src/hidkit_debug.h）。
 * 1 = 打冷路径自证信息（认领判定/解析失败/挤出/首次丢弃），需要你自己实现
 *     hidkit_debug_printf 才能看到东西；0 = 连调用点一起裁掉（默认）。 */
#ifndef HIDKIT_DEBUG
#define HIDKIT_DEBUG 0
#endif

/* 时间源（毫秒、单调递增），用于 HIDKIT_OVERFLOW_EVICT_IDLE 判断"最久没有报文"。
 * 未定义时退化为"挤掉最老的槽位"，不依赖任何时间函数。 */
/* #define HIDKIT_TICK_MS() my_millis() */

/* ---- 平台标注 ---- */
/* 把**每报文都要跑**的函数放进 RAM（Pico 上主核写 flash 期间 XIP 停摆，
 * 留在 flash 的函数会卡在这段窗口里）。标注范围是入口 + dispatch/parse 层；
 * 挂载期解析描述符、查询类辅助函数不标。
 *
 * 默认展开为空，任何平台都能编译。展开式必须**连标识符一起吐出来**
 * （属性 + 名字），与 SDK 的 __not_in_flash_func 同构 —— 只给属性不给名字
 * 会在 `bool HIDKIT_HOT(foo)(int)` 处直接编译报错。static inline 的布局解析
 * 函数也照标不误：它们地址被取走，必然发射出独立函数体。
 *
 *   #define HIDKIT_HOT(f) __not_in_flash_func(f)                            // Pico SDK
 *   #define HIDKIT_HOT(f) __attribute__((section(".time_critical.hidkit"))) f // 裸属性
 *
 * 注意本库只能标自己这半边：热路径的最后一步是 host 侧实现的那几个
 * hidkit_input_* 弱符号，它们要由宿主自己放进 RAM，整条链才真正不停摆。 */
#ifndef HIDKIT_HOT
#define HIDKIT_HOT
#endif

#endif /* HIDKIT_CONFIG_H */
