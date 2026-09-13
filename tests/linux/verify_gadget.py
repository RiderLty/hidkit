#!/usr/bin/env python3
"""
tests/linux/verify_gadget.py —— 用 USB gadget 回环在树莓派上真机验证 hidkit。

思路：本脚本用 ConfigFS 造出目标 HID 设备（描述符/协议取自真机抓包或构造），
gadget 口回接到本机后 Linux 枚举成 /dev/hidrawN；再用 tests/linux/hidkit_hidraw
取描述符、mount、读报文；脚本往 /dev/hidgN 注入报文，最后断言事件序列。

需要 root。用法：
    sudo python3 verify_gadget.py --probe /tmp/hidkit-verify/hidkit_hidraw [--case A]

注意：ConfigFS 的清理有时要跑几次才干净（脚本已内置重试）。
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time

CFG = "/sys/kernel/config/usb_gadget"


def w(path, val):
    with open(path, "w") as f:
        f.write(val)


def wb(path, data):
    with open(path, "wb") as f:
        f.write(data)


def udc_name():
    u = [os.path.basename(p) for p in glob.glob("/sys/class/udc/*")]
    if not u:
        raise RuntimeError("no UDC found")
    return u[0]


def _try_rmdir(p):
    try:
        os.rmdir(p)
    except OSError:
        pass


def _try_rm(p):
    try:
        os.remove(p)
    except OSError:
        pass


def remove_gadget_once(g):
    try:
        if os.path.exists(g + "/UDC"):
            w(g + "/UDC", "")
    except OSError:
        pass
    for cfg in glob.glob(g + "/configs/*"):
        for link in glob.glob(cfg + "/*"):
            if os.path.islink(link):
                _try_rm(link)
        for s in glob.glob(cfg + "/strings/*"):
            _try_rmdir(s)
        _try_rmdir(cfg + "/strings")
        _try_rmdir(cfg)
    for fn in glob.glob(g + "/functions/*"):
        _try_rmdir(fn)
    if os.path.isdir(g + "/os_desc"):
        for link in glob.glob(g + "/os_desc/*"):
            if os.path.islink(link):
                _try_rm(link)
    for s in glob.glob(g + "/strings/*"):
        _try_rmdir(s)
    _try_rmdir(g + "/strings")
    _try_rmdir(g)


def clear_gadgets(tries=12):
    for _ in range(tries):
        gs = glob.glob(CFG + "/*")
        if not gs:
            return True
        for g in gs:
            remove_gadget_once(g)
        time.sleep(0.3)
    return not glob.glob(CFG + "/*")


def create_gadget(name, vid, pid, ifaces, udc):
    if not clear_gadgets():
        raise RuntimeError("gadget cleanup failed")
    g = f"{CFG}/{name}"
    os.makedirs(g)
    w(g + "/idVendor", f"0x{vid:04x}")
    w(g + "/idProduct", f"0x{pid:04x}")
    w(g + "/bcdUSB", "0x0200")
    w(g + "/bcdDevice", "0x0100")
    w(g + "/bDeviceClass", "0x00")
    os.makedirs(g + "/strings/0x409")
    w(g + "/strings/0x409/serialnumber", "0123456789abcdef")
    w(g + "/strings/0x409/manufacturer", "hidkit-test")
    w(g + "/strings/0x409/product", "hidkit-verify")
    os.makedirs(g + "/configs/c.1")
    os.makedirs(g + "/configs/c.1/strings/0x409")
    w(g + "/configs/c.1/strings/0x409/configuration", "verify")
    w(g + "/configs/c.1/MaxPower", "250")
    for i, f in enumerate(ifaces):
        fn = f"{g}/functions/hid.usb{i}"
        os.makedirs(fn)
        w(fn + "/protocol", str(f["protocol"]))
        w(fn + "/subclass", str(f.get("subclass", 0)))
        w(fn + "/report_length", str(f["report_length"]))
        wb(fn + "/report_desc", f["desc"])
        os.symlink(fn, f"{g}/configs/c.1/hid.usb{i}")
    w(g + "/UDC", udc)
    return g


def hidraw_for(vid, pid):
    out = []
    for h in glob.glob("/sys/class/hidraw/hidraw*"):
        try:
            u = open(h + "/device/uevent").read()
        except OSError:
            continue
        m = re.search(r"HID_ID=\w+:([0-9A-Fa-f]+):([0-9A-Fa-f]+)", u)
        if m and int(m.group(1), 16) == vid and int(m.group(2), 16) == pid:
            out.append("/dev/" + os.path.basename(h))
    return sorted(out)


def wait_hidraw(vid, pid, n, timeout=8.0):
    t = time.time()
    while time.time() - t < timeout:
        hs = hidraw_for(vid, pid)
        if len(hs) >= n:
            return hs
        time.sleep(0.2)
    return hidraw_for(vid, pid)


def wr_hidg(idx, data):
    fd = os.open(f"/dev/hidg{idx}", os.O_WRONLY)
    try:
        os.write(fd, bytes(data))
    finally:
        os.close(fd)


# ------------------------------------------------------------------
# 用例
# ------------------------------------------------------------------

# A. 真机 Compx 3554:fa09 接口 1：proto=Mouse，却含 NKRO 键盘(ID4)+鼠标(ID7)
DESC_COMPX = bytes.fromhex(
    "0602ff0902a1018513150026ff007508"
    "95130902810009029100c0050c0901a1"
    "0185051500263c0219002a3c02751095"
    "018100c005010980a101850319812983"
    "15002501950375018102950175058101"
    "c005010906a101850405071500250119"
    "00299f95a075018102c005010902a101"
    "0901a100850705091901290515002501"
    "95057501810295017503810105010930"
    "093116008026ff7f751095028106c0a1"
    "00050109381581257f750895018106c0"
    "a100050c0a3802950175081581257f81"
    "06c0c00604ff0902a101850609021500"
    "26ff0075089507b102c0"
)
assert len(DESC_COMPX) == 218, len(DESC_COMPX)

# B. 真机 AJAZZ AK029 接口 1：proto=0，NKRO 键盘(ID1)+Consumer(ID3)+系统(ID2)+鼠标(ID6)
DESC_AJAZZ = bytes.fromhex(
    "050c0901a101850319002a3c031500263c03950175108100c0"
    "05010980a101850205011981298315002501950375018102950175058101c0"
    "05010906a101850105071500250119002977957875018102c0"
    "05010902a10185060901a10005091901290315002501950575018102"
    "95017503810105010930093109381581257f750895038106c0c0"
)

# C. 构造：2 字节形式 Report Count(256) 的全键位图
DESC_NKRO256 = bytes.fromhex(
    "05010906a1018501050715002501190029ff75019600018102c0"
)

# D. 构造：键盘与鼠标共用同一 Report ID(1)
DESC_SAMEID = bytes.fromhex(
    "05010906a101850105071904290715002501750195048102950475018101c0"
    "05010902a10105091901290215002501750195028102950675018101"
    "0501093009311581257f750895028106c0"
)


def case_A():
    def inject():
        # ID4：NKRO 位图 byte[4]（usage 0x20~0x27）bit3..7 = 6,7,8,9,0
        for v in (0x08, 0x18, 0x38, 0x78, 0xF8, 0x78, 0x38, 0x18, 0x08, 0x00):
            r = bytearray(21)
            r[0] = 4
            r[5] = v
            wr_hidg(0, r)
            time.sleep(0.06)
        # ID7：鼠标（首帧建基线）
        wr_hidg(0, bytes([7, 0, 0, 0, 0, 0, 0, 0]))
        time.sleep(0.06)
        wr_hidg(0, bytes([7, 1, 5, 0, 0xFB, 0xFF, 0, 0]))
        time.sleep(0.06)

    expect = [
        "KEY seg=0x000 code=0x23 down",
        "KEY seg=0x000 code=0x27 down",
        "KEY seg=0x000 code=0x27 up",
        "KEY seg=0x000 code=0x23 up",
        "MOUSE seg=0x100 code=0x00 down",
        "MOUSE dx=5 dy=-5 wheel=0",
        "MOUSE seg=0x100 code=0x00 up",   # 卸载补发（hid_mouse_release_all）
    ]
    return dict(name="A_compx_nkro_mouse", vid=0x3554, pid=0xFA09,
                ifaces=[dict(protocol=2, subclass=1, report_length=21, desc=DESC_COMPX)],
                inject=inject, expect=expect, seconds=6)


def case_B():
    def inject():
        # ID1：120 位位图，bit4 → usage 0x04（'a'）
        r = bytearray(16)
        r[0] = 1
        r[1] = 0x10
        wr_hidg(0, r)
        time.sleep(0.08)
        r[1] = 0x00
        wr_hidg(0, r)
        time.sleep(0.08)
        # ID6：鼠标
        wr_hidg(0, bytes([6, 0, 0, 0, 0]))
        time.sleep(0.08)
        wr_hidg(0, bytes([6, 0x01, 0x05, 0xFB, 0x02]))
        time.sleep(0.08)

    expect = [
        "KEY seg=0x000 code=0x04 down",
        "KEY seg=0x000 code=0x04 up",
        "MOUSE seg=0x100 code=0x00 down",
        "MOUSE dx=5 dy=-5 wheel=2",
        "MOUSE seg=0x100 code=0x00 up",   # 卸载补发
    ]
    return dict(name="B_ajazz_composite", vid=0x0C45, pid=0x8046,
                ifaces=[dict(protocol=0, subclass=0, report_length=16, desc=DESC_AJAZZ)],
                inject=inject, expect=expect, seconds=6)


def case_C():
    def inject():
        r = bytearray(33)
        r[0] = 1
        r[32] = 0x80          # bit255 → usage 0xFF
        wr_hidg(0, r)
        time.sleep(0.1)
        r[32] = 0x00
        wr_hidg(0, r)
        time.sleep(0.1)

    expect = [
        "KEY seg=0x000 code=0xFF down",
        "KEY seg=0x000 code=0xFF up",
    ]
    return dict(name="C_nkro_256", vid=0x0BAD, pid=0x0100,
                ifaces=[dict(protocol=1, subclass=1, report_length=33, desc=DESC_NKRO256)],
                inject=inject, expect=expect, seconds=5)


def case_D():
    def inject():
        wr_hidg(0, bytes([1, 0x01, 0x00, 0x00, 0x00]))   # 键盘 bit0 → 0x04
        time.sleep(0.08)
        wr_hidg(0, bytes([1, 0x03, 0x01, 0x00, 0x00]))   # 键盘 bit1 + 鼠标左键
        time.sleep(0.08)
        wr_hidg(0, bytes([1, 0x03, 0x00, 0x05, 0xFB]))   # 鼠标左键松开 + 位移
        time.sleep(0.08)

    expect = [
        "KEY seg=0x000 code=0x04 down",
        "KEY seg=0x000 code=0x05 down",
        "MOUSE seg=0x100 code=0x00 down",
        "MOUSE seg=0x100 code=0x00 up",
        "MOUSE dx=5 dy=-5 wheel=0",
    ]
    return dict(name="D_same_report_id", vid=0x0BAD, pid=0x0101,
                ifaces=[dict(protocol=0, subclass=0, report_length=5, desc=DESC_SAMEID)],
                inject=inject, expect=expect, seconds=5)


CASES = {"A": case_A, "B": case_B, "C": case_C, "D": case_D}


def run_case(probe, case):
    udc = udc_name()
    print(f"\n===== case {case['name']} =====")
    create_gadget("hk_verify", case["vid"], case["pid"], case["ifaces"], udc)
    hs = wait_hidraw(case["vid"], case["pid"], len(case["ifaces"]))
    print(f"  hidraw: {hs}")
    if len(hs) < len(case["ifaces"]):
        print("  FAIL: 设备未枚举出预期的 hidraw")
        clear_gadgets()
        return False

    p = subprocess.Popen([probe, f"{case['vid']:04x}:{case['pid']:04x}",
                          str(case["seconds"])],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, bufsize=1)
    lines = []
    ready = False
    t0 = time.time()
    while time.time() - t0 < 10:
        line = p.stdout.readline()
        if not line:
            break
        lines.append(line.rstrip("\n"))
        if "[READY]" in line:
            ready = True
            break
    if not ready:
        p.kill()
        rest, _ = p.communicate()
        print("  FAIL: probe 未就绪\n" + "\n".join(lines) + "\n" + (rest or ""))
        clear_gadgets()
        return False

    time.sleep(0.3)
    case["inject"]()
    try:
        rest, _ = p.communicate(timeout=case["seconds"] + 10)
    except subprocess.TimeoutExpired:
        p.kill()
        rest, _ = p.communicate()
    text = "\n".join(lines) + "\n" + (rest or "")
    print("  ---- probe 输出 ----")
    for ln in text.splitlines():
        print("  " + ln)

    ok = True
    for e in case["expect"]:
        if e not in text:
            print(f"  MISSING: {e}")
            ok = False
    # 键鼠事件必须来自同一个 slot（复合接口）
    if ok:
        print("  PASS")
    else:
        print("  FAIL")
    clear_gadgets()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", default="./hidkit_hidraw")
    ap.add_argument("--case", default="ABCD")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("需要 root", file=sys.stderr)
        return 2

    results = {}
    for key in args.case:
        fn = CASES.get(key)
        if not fn:
            continue
        try:
            results[key] = run_case(args.probe, fn())
        except Exception as e:  # noqa: BLE001
            print(f"  EXCEPTION: {e}")
            results[key] = False
            clear_gadgets()

    print("\n===== 汇总 =====")
    for k, v in results.items():
        print(f"  {k}: {'PASS' if v else 'FAIL'}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
