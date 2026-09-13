# tests/linux —— 真机（USB gadget 回环）验证

主机侧样本测试（`tests/host/`）只喂"描述符 + 报文"，跑不到真实 USB 栈。
这里用 **Linux USB Gadget** 造出目标 HID 设备，再把 gadget 口回接到本机
（树莓派 5 的 gadget 口插到自己上即可），于是 hidkit 可以在**真实枚举**下验证：

```
ConfigFS 造 gadget → gadget 口回接 → 本机枚举出 /dev/hidrawN
                                          │
             /dev/hidgN ←注入报文          ▼
                       └──────────► hidkit_hidraw: HIDIOCGRDESC 取描述符
                                      → hidkit_mount → 读 hidraw → hidkit_report
```

## 环境

- Linux 主机，内核支持 ConfigFS + `usb_f_hid`（树莓派 5 已带）
- gadget 口与主机自己的 USB 口连通（rp5 是直接回接）
- 需要 root

## 构建

```bash
gcc -O2 -Wall -Wextra -Werror -Iinclude -Isrc \
    -o hidkit_hidraw tests/linux/hidkit_hidraw.c src/*.c
```

## 运行

```bash
sudo python3 tests/linux/verify_gadget.py --probe ./hidkit_hidraw        # 全部用例
sudo python3 tests/linux/verify_gadget.py --probe ./hidkit_hidraw --case A
```

脚本会：清理残留 gadget（ConfigFS 清理有时要重试，脚本内已循环多次）
→ 建 gadget → 等枚举 → 起 probe → 往 `/dev/hidgN` 注入报文 → 断言事件序列
→ 清理。退出码 0 = 全过。

单独手动看某个设备：

```bash
sudo ./hidkit_hidraw 3554:fa09 6      # 6 秒；第 3 个参数可指定 proto
```

输出：`[DSC]` 描述符/分类、`[RX]` 原始报文、`[EV]` hidkit 事件、
`SUMMARY key=.. mouse=.. gp=..`。

## 用例

| 用例 | 设备 | 验证点 |
|---|---|---|
| A | 真机 Compx `3554:fa09` 接口 1（proto=Mouse，却含 NKRO 键盘 ID4 + 鼠标 ID7） | 描述符优先分类；NKRO 位图键 6~0；同接口鼠标；卸载补发 |
| B | 真机 AJAZZ AK029 接口 1（proto=0，NKRO 键盘 ID1 + Consumer + 系统 + 鼠标 ID6） | 不做 boot 假设；120 位位图；同接口鼠标 |
| C | 构造：2 字节 `Report Count(256)` 全键位图 | 16 位计数不被截断（bit255 → usage 0xFF） |
| D | 构造：键盘与鼠标共用同一 Report ID | 同 ID 键鼠并行解析 |

## 这套验证抓到的真机问题

- **描述符鼠标卸载不补发按住的按键**：`slot_release()` 原来只调
  `hid_nkro_release_all()` 和 `hid_dispatch_reset()`，而描述符鼠标的按键状态在
  `s_mouse[].last_buttons`，于是"按着左键拔掉设备"时上层永远等不到松开。
  现在 `CAP_MOUSE_DESC` 会调 `hid_mouse_release_all()`（用例 A/B 的
  `MOUSE ... up` 就是卸载补发）。样本侧对应 `test_mouse_umount_release`。

## 备注

- 设备处于 report protocol 时，hidraw 读到的就是含 Report ID 的原始报文，
  与 hidkit 的期望一致；boot protocol 的固定格式设备同样能读到原始字节。
- gadget 的 `report_length` 取该接口所有 Report ID 里的最大报文长度。
- 这里直接用 ConfigFS，没有走 `linux-cu`；后者的清理偶发不干净，ConfigFS 的
  移除循环重试几次即可。
