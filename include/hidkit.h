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

/*--------------------------------------------------------------------+
 * 触摸板通用快照（DS5/DS Edge 起步，将来的触摸板设备共用同一格式）
 *
 * 快照 = 两个 32 位元字 + N 个坐标对；签名一次定格，设备差异全在字段里。
 * 单个 32 位字装不下"10 触点 + 宽 + 高"（10+12+12 = 34 > 32），故拆两字。
 *--------------------------------------------------------------------*/

#define HIDKIT_TOUCHPAD_SLOTS 10   /* 编译期常量（非配置开关）：坐标对按槽位
                                    * 顺序尾部追加，改小不影响已写的消费方脚本 */

/* meta bit14-19 来源枚举（bit20-31 保留）。不发 PID/VID：触摸板场景
 * 宽高 + 类型已覆盖全部实际需求，枚举仅为"同尺寸不同设备"留身份位。
 * 新枚举按 1、2、3… 顺序分配，登记处 = 各布局的 parse。 */
#define HIDKIT_TPSRC_DS5       0x00u   /* DS5 / DS Edge（1920 × 943，12bit 坐标） */

#define HIDKIT_TP_META_CONTACTS_MASK  0x000003FFu  /* bit0-9 触点 i 在位 */
#define HIDKIT_TP_META_SLOTS_SHIFT    10           /* bit10-13 最大点数 0..15 */
#define HIDKIT_TP_META_SLOTS_MASK     0x00003C00u
#define HIDKIT_TP_META_SOURCE_SHIFT   14           /* bit14-19 来源枚举 */
#define HIDKIT_TP_META_SOURCE_MASK    0x000FC000u

typedef struct {
    uint32_t meta;                    /* 位域见上 */
    uint32_t dims;                    /* bit0-15 宽 W ｜ bit16-31 高 H（设备原生） */
    uint16_t x[HIDKIT_TOUCHPAD_SLOTS];
    uint16_t y[HIDKIT_TOUCHPAD_SLOTS]; /* 无效槽位恒 0 */
} hidkit_touchpad_t;

/* 键盘 / 鼠标按键 / 手柄按键统一走这里，用 HIDKIT_CODE_* 段前缀区分类型。
 * 仅在状态变化时回调（边沿由库内检测）。 */
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed);

/* 鼠标位移与滚轮（仅鼠标；不做横向滚轮）。仅在非零时回调。 */
void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel);

/* 手柄绝对状态：每份解析成功的报文都会回调（不做去重 —— 手柄报文本身
 * 就是当前绝对状态）。buttons = 手柄原生位图（第 i 位 = btn_map[i]），
 * 键类变化另有 key 事件；这里带全量位图是为了让消费方可以整帧显示。 */
void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt,
                              uint32_t buttons);

/* 手柄六轴运动数据（陀螺仪 + 加速度计）：每份含六轴的解析成功报文都会回调
 * （不做去重）。仅带 IMU 的手柄（如 DS5/DS Edge）会回调；无 IMU 的手柄
 * （XInput/Azeron）不发 —— 0 对陀螺仪是合法读数（"完全静止"），发 0 是
 * 伪造读数。设备卸载时会补发一次全零（消费方没有 umount 通知，补零就是
 * "设备已离开"的信号；与按钮补松开、摇杆归零同一语义）。
 *
 * 量程约定（基准 = DS5 满量程，与摇杆 -32767..32767 约定同构）：
 *   陀螺仪：±32767 = ±HIDKIT_MOTION_GYRO_FULL_SCALE_DPS (±2000 dps)
 *   加速度：±32767 = ±4 g（1 g ≈ 8192）；静止时含重力分量（正面朝上
 *           重力模长 ≈8192），重力补偿是消费方策略，库内不做。
 *   DS5 / DS Edge 使用 PlayStation 原生轴序；Switch Pro 六轴统一转换为
 *   此坐标系（native X/Y/Z → output -Y/Z/-X）。Joy-Con 保留原生坐标系。
 *   角速度 X=俯仰 pitch、Y=偏航 yaw、Z=横滚 roll。 */
void hidkit_input_gamepad_motion(int8_t slot, int16_t gx, int16_t gy, int16_t gz,
                                 int16_t ax, int16_t ay, int16_t az);

/* 触摸板快照（通用格式，DS5/DS Edge 及将来的触摸板设备共用此通道）：
 * 每份解析成功的报文都会回调（不做去重 —— 静止手指的定频报文由消费方
 * 去重，库内不理解内容）。设备卸载时补发一次全零快照（"设备已离开"信号，
 * 与六轴补零同一语义）。
 *
 * meta 位域（bit0-9 触点在位；bit10-13 设备最大点数；bit14-19 来源枚举；
 * bit20-31 保留，恒 0）。坐标 = 设备原生分辨率，库内零换算（宽高比保真），
 * 无效槽位坐标恒 0。分辨率等设备常量随帧携带（dims），消费方无需查表。
 * 不透传 tracking counter：同槽位换手指 = 触点位 down→up→down 序列可见。 */
void hidkit_input_touchpad(int8_t slot, const hidkit_touchpad_t *tp);

/* 可选诊断：设备被丢弃时（槽位耗尽且策略为 DROP_NEW）通知一次。 */
void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid);

/*--------------------------------------------------------------------+
 * 手柄布局：设备原生位 + 位→BTN_* 查表（HID 手柄与 XInput 共用同一结构）
 *--------------------------------------------------------------------*/

/* 六轴量程锚点（只做文档与换算参照，库内不参与运算）：非 DS5 布局按
 *   gyro  = dps  * 32767 / HIDKIT_MOTION_GYRO_FULL_SCALE_DPS
 *   accel = mg   * 32767 / HIDKIT_MOTION_ACCEL_FULL_SCALE_MG
 * 缩放到 -32767..32767。 */
#define HIDKIT_MOTION_GYRO_FULL_SCALE_DPS   2000
#define HIDKIT_MOTION_ACCEL_FULL_SCALE_MG   4000   /* 1 g ≈ 8192 unit */

typedef struct {
    int32_t  ls_x, ls_y, rs_x, rs_y;  /* -32767..32767 */
    int32_t  lt, rt;                  /* 0..32767 */
    int16_t  gyro_x, gyro_y, gyro_z;  /* ±32767 = ±2000 dps（量程约定见上） */
    int16_t  accel_x, accel_y, accel_z; /* ±32767 = ±4 g，静止时含重力 */
    uint8_t  has_motion;              /* 本报文含有效六轴数据（布局声明） */
    uint8_t  has_touchpad;            /* 布局声明含触摸板（本报文 touchpad 有效） */
    hidkit_touchpad_t touchpad;       /* 仅 has_touchpad=1 时有效 */
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
 * Switch Pro 校准数据注入（057e:2009/2006/2007）
 *
 * 手柄的摇杆/六轴出厂即未校准，工厂/用户校准存在手柄 SPI flash 里，
 * 由宿主（握手模块）经子命令 0x10 读回后注入。地址与解码依据
 * dekuNukem/Nintendo_Switch_Reverse_Engineering 与 Linux hid-nintendo：
 *   - 摇杆 12bit 原始值域 0..4095，min/max 为**相对中心的偏移**；
 *   - IMU 先按每轴 SPI 灵敏度换算为物理量，再归一化并限幅：
 *     gyro_dps = (raw - origin) * 936 / (scale - origin)；
 *     accel_g = raw * 4 / (scale - origin)，保留重力。
 *     无校准时 scale 缺省为 gyro=13371、accel=16384。
 * 未注入（或未含对应 valid 位）时布局用缺省映射（中心 2048、半量程 1024、
 * 零偏 0）——流先出来，校准到位后自动变准。
 *--------------------------------------------------------------------*/

#define HIDKIT_SWITCHPRO_CAL_STICKS 0x01u
#define HIDKIT_SWITCHPRO_CAL_MOTION 0x02u

typedef struct {
    uint16_t valid;        /* HIDKIT_SWITCHPRO_CAL_* 位组合 */
    /* 摇杆：中心绝对值 + 满偏（相对中心的偏移，12bit） */
    uint16_t ls_xc, ls_yc, ls_xmax, ls_xmin, ls_ymax, ls_ymin;
    uint16_t rs_xc, rs_yc, rs_xmax, rs_xmin, rs_ymax, rs_ymin;
    /* SPI origin：acc 用于灵敏度分母，gyro 同时作为静止零偏 */
    int16_t  acc_origin[3];
    int16_t  gyro_origin[3];
    int16_t  acc_hoffset[3]; /* 平放参考值（含重力），仅保留元数据，不从输出扣除 */
    int16_t  acc_scale[3];  /* SPI +6：灵敏度系数，缺省 16384 */
    int16_t  gyro_scale[3]; /* SPI +18：灵敏度系数，缺省 13371 */
} hidkit_switchpro_calib_t;

/* 注入校准（按槽位）。槽位未由本库持有 / 非手柄 / 数据不合理（中心越界、
 * 满偏为 0）返回 false。卸载与槽位复用时库内自动失效。 */
bool hidkit_switchpro_set_calib(int8_t slot, const hidkit_switchpro_calib_t *cal);

/*--------------------------------------------------------------------+
 * 入口
 *--------------------------------------------------------------------*/

/* 初始化：清空所有槽位与解析状态（可重复调用）。出口函数是弱符号，无需注册。 */
void hidkit_init(void);

/*
 * 设备挂载：判定本库是否接管该接口。
 *
 * 判定是**描述符优先**的：只要拿到报告描述符，就按描述符解析出的能力集认领
 * （键盘键位段 / 鼠标字段 / VID:PID 手柄布局），一个接口可以同时具备多种能力
 * （多 Report ID 复合描述符，典型如「键盘 + 鼠标」复合的带宏键盘）；bInterfaceProtocol
 * 只在描述符缺失或描述符里没有本库认识的集合时作兜底。鼠标能力按 proto==Mouse
 * 或描述符里的 Mouse/Pointer 顶层集合认领，因此把带 NKRO / 鼠标集合、却谎报
 * proto 的复合接口交进来时，本库仍能正确路由报文。
 *
 * 返回 >= 0：本库已接管，后续用该 slot 调用 hidkit_report / hidkit_umount。
 * 返回 HIDKIT_UNHANDLED：本库不认识该设备，宿主可自行处理（扩展点）。
 * 返回 HIDKIT_ERR_NO_SLOT：认识但槽位耗尽（策略为 DROP_NEW 时）。
 */
int8_t hidkit_mount(const hidkit_dev_info_t *dev);

/*
 * 收到报文：解析并触发回调。
 * 返回 true = 已消费；false = 未消费（该 slot 不属于本库，或该设备类型未启用，
 * 或该接口虽已认领但这份报文的 Report ID 不属于任何已解析集合 —— 如厂商报表）。
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

/* 槽位主类型查询（宿主的设备清单/面板展示用）：返回 HIDKIT_SLOTKIND_*。
 * slot 越界或空闲返回 HIDKIT_SLOTKIND_NONE。判定以报告描述符语义为准，
 * 比宿主侧的 boot 接口协议声明（tuh_hid_interface_protocol）可靠。 */
enum {
    HIDKIT_SLOTKIND_NONE = 0,
    HIDKIT_SLOTKIND_MOUSE,
    HIDKIT_SLOTKIND_KEYBOARD,
    HIDKIT_SLOTKIND_GAMEPAD,
    HIDKIT_SLOTKIND_NKRO,
};
uint8_t hidkit_slot_kind(int8_t slot);

/* 未接管返回值 */
#define HIDKIT_UNHANDLED   (-1)
#define HIDKIT_ERR_NO_SLOT (-2)

#ifdef __cplusplus
}
#endif

#endif /* HIDKIT_H */
