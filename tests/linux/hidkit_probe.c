/*
 * tests/linux/hidkit_probe.c —— 在 Linux 上验证**真实**键鼠/手柄的解析
 *
 * 它把真实 HID 设备的报告描述符与原始报文，按嵌入式 host 栈的路径喂给 hidkit：
 *
 *   真实设备 → 内核 usbhid → /dev/hidrawN
 *                              ├─ ioctl(HIDIOCGRDESC) → hidkit_mount()
 *                              └─ read()               → hidkit_report() → 事件
 *
 * 用法（/dev/hidraw* 是 root only，需 sudo）：
 *   sudo ./hidkit_probe --list                      列出设备与 hidkit 分类
 *   sudo ./hidkit_probe --vidpid 046d:c08b          选中该 VID:PID
 *   sudo ./hidkit_probe --index 0 --dump-desc       选中列表里的第 0 组并 dump 描述符
 *   sudo ./hidkit_probe --name "Wireless"           按名称子串选中
 *   sudo ./hidkit_probe --seconds 10 --raw          跑 10 秒并打印原始报文
 *   sudo ./hidkit_probe                             只有一个设备时直接用；否则提示选择
 *
 * 参数：
 *   --list              只列出，不监听
 *   --vidpid V:P        选中 VID:PID
 *   --index N           选中 --list 里的第 N 组（可逗号分隔多个）
 *   --name SUBSTR       按 HID_NAME 子串选中
 *   --proto N           强制 mount 的 proto（默认 0 = 描述符优先）
 *   --seconds N         监听时长（默认 0 = 直到 Ctrl-C）
 *   --raw               打印每份原始报文
 *   --dump-desc         打印报告描述符逐 item 解析 + hidkit 解析结论
 *   --mouse-ms N        鼠标位移聚合打印间隔毫秒（默认 100；0 = 每份都打）
 *   --no-warn           不打印“设备可能被内核占用”之类的提示
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/hidraw.h>

#include "hidkit.h"
#include "hid_parser.h"   /* 内部解析器：用于 --list/--dump-desc 的分类与 span 展示 */

#define MAX_IFACES 64
#define MAX_GROUPS 64
#define DESC_MAX   4096

/*--------------------------------------------------------------------+
 * 设备枚举
 *--------------------------------------------------------------------*/

typedef struct {
    char     dev[300];        /* /dev/hidrawN */
    char     sys[320];       /* /sys/class/hidraw/hidrawN */
    char     group[256];     /* USB 物理设备分组键（HID_PHYS 去掉 /inputN） */
    char     name[128];      /* HID_NAME */
    uint16_t vid, pid;
    int      input_idx;      /* inputN 的 N（≈ 接口序号） */
    int      proto_real;     /* 从 USB 接口 sysfs 读到的 bInterfaceProtocol（-1=未知） */
    uint8_t  desc[DESC_MAX];
    uint32_t desc_len;
    bool     desc_ok;
    int      fd;
    int8_t   slot;
} itf_t;

typedef struct {
    char     key[256];
    char     name[128];
    uint16_t vid, pid;
    int      n;
    int      idx[MAX_IFACES];
} group_t;

static itf_t   g_itf[MAX_IFACES];
static int     g_nitf;
static group_t g_grp[MAX_GROUPS];
static int     g_ngrp;

static volatile sig_atomic_t g_stop;

static void on_sigint(int sig) { (void)sig; g_stop = 1; }

static void copy_str(char *dst, size_t n, const char *src)
{
    if (n == 0) return;
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static bool file_first_line(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = '\0';
    return true;
}

/* /sys/class/hidraw/hidrawN/device/uevent 里取 HID_ID / HID_NAME / HID_PHYS */
static bool read_uevent(const char *sys, uint16_t *vid, uint16_t *pid,
                        char *name, size_t name_n, char *phys, size_t phys_n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/device/uevent", sys);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[512];
    unsigned v = 0, p = 0;
    phys[0] = '\0';
    name[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "HID_ID=", 7) == 0) {
            unsigned bus = 0;
            if (sscanf(line + 7, "%x:%x:%x", &bus, &v, &p) == 3) { /* ok */ }
        } else if (strncmp(line, "HID_NAME=", 9) == 0) {
            copy_str(name, name_n, line + 9);
            size_t l = strlen(name);
            while (l && (name[l - 1] == '\n' || name[l - 1] == '\r')) name[--l] = '\0';
        } else if (strncmp(line, "HID_PHYS=", 9) == 0) {
            copy_str(phys, phys_n, line + 9);
            size_t l = strlen(phys);
            while (l && (phys[l - 1] == '\n' || phys[l - 1] == '\r')) phys[--l] = '\0';
        }
    }
    fclose(f);
    *vid = (uint16_t)v;
    *pid = (uint16_t)p;
    return true;
}

/* 从 USB 接口的 sysfs 读 bInterfaceProtocol（hidraw 的 device 上级目录） */
static int read_itf_protocol(const char *sys)
{
    char p[1024], rp[1024];
    snprintf(p, sizeof(p), "%s/device", sys);
    if (!realpath(p, rp)) return -1;
    char *slash = strrchr(rp, '/');
    if (!slash) return -1;
    *slash = '\0';
    char f[1088];
    snprintf(f, sizeof(f), "%s/bInterfaceProtocol", rp);
    char line[32];
    if (!file_first_line(f, line, sizeof(line))) return -1;
    return atoi(line);
}

static bool read_report_desc(const char *dev, uint8_t *buf, uint32_t *len){
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) return false;

    struct hidraw_report_descriptor rpt;
    memset(&rpt, 0, sizeof(rpt));
    if (ioctl(fd, HIDIOCGRDESCSIZE, &rpt.size) < 0 ||
        ioctl(fd, HIDIOCGRDESC, &rpt) < 0) {
        close(fd);
        return false;
    }
    uint32_t n = rpt.size;
    if (n > *len) n = *len;
    memcpy(buf, rpt.value, n);
    *len = n;
    close(fd);
    return true;
}

static void enumerate(void)
{
    g_nitf = 0;
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_nitf < MAX_IFACES) {
        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;

        itf_t *t = &g_itf[g_nitf];
        memset(t, 0, sizeof(*t));
        t->fd = -1;
        t->slot = -1;
        t->proto_real = -1;
        snprintf(t->sys, sizeof(t->sys), "/sys/class/hidraw/%s", de->d_name);
        snprintf(t->dev, sizeof(t->dev), "/dev/%s", de->d_name);

        char phys[256];
        if (!read_uevent(t->sys, &t->vid, &t->pid, t->name, sizeof(t->name),
                         phys, sizeof(phys))) {
            continue;
        }
        t->proto_real = read_itf_protocol(t->sys);
        /* 分组键：HID_PHYS = "usb-xhci-hcd.1-1/input0" → "usb-xhci-hcd.1-1" */
        copy_str(t->group, sizeof(t->group), phys);
        char *slash = strrchr(t->group, '/');
        if (slash) *slash = '\0';
        const char *ip = strstr(phys, "input");
        t->input_idx = ip ? atoi(ip + 5) : -1;

        t->desc_len = sizeof(t->desc);
        t->desc_ok = read_report_desc(t->dev, t->desc, &t->desc_len);
        g_nitf++;
    }
    closedir(d);
}

static void groupify(void)
{
    g_ngrp = 0;
    for (int i = 0; i < g_nitf; i++) {
        int g = -1;
        for (int j = 0; j < g_ngrp; j++) {
            if (strcmp(g_grp[j].key, g_itf[i].group) == 0) { g = j; break; }
        }
        if (g < 0 && g_ngrp < MAX_GROUPS) {
            g = g_ngrp++;
            memset(&g_grp[g], 0, sizeof(g_grp[g]));
            copy_str(g_grp[g].key, sizeof(g_grp[g].key), g_itf[i].group);
            copy_str(g_grp[g].name, sizeof(g_grp[g].name), g_itf[i].name);
            g_grp[g].vid = g_itf[i].vid;
            g_grp[g].pid = g_itf[i].pid;
        }
        if (g >= 0 && g_grp[g].n < MAX_IFACES) {
            g_grp[g].idx[g_grp[g].n++] = i;
        }
    }
}

/*--------------------------------------------------------------------+
 * 描述符分类（调用 hidkit 内部解析器，只为显示）
 *--------------------------------------------------------------------*/

static void appendf(char *buf, size_t n, int *off, const char *fmt, ...)
{
    if (*off >= (int)n) return;
    va_list ap;
    va_start(ap, fmt);
    int wr = vsnprintf(buf + *off, n - (size_t)*off, fmt, ap);
    va_end(ap);
    if (wr > 0) *off += wr;
    if (*off > (int)n) *off = (int)n;
}

static void describe(const itf_t *t, char *out, size_t n)
{
    out[0] = '\0';
    if (!t->desc_ok) { copy_str(out, n, "描述符读取失败"); return; }

    hid_nkro_desc_t nd;
    hid_mouse_desc_t md;
    memset(&nd, 0, sizeof(nd));
    memset(&md, 0, sizeof(md));
    bool kb = hid_nkro_parse(&nd, t->desc, (uint16_t)t->desc_len);
    bool ms = hid_mouse_parse(&md, t->desc, (uint16_t)t->desc_len);
    bool mcol = hid_desc_has_mouse_collection(t->desc, (uint16_t)t->desc_len);

    int off = 0;
    if (kb) {
        appendf(out, n, &off, "keyboard(NKRO spans=%u)", (unsigned)nd.num_spans);
    }
    if (ms || mcol) {
        appendf(out, n, &off, "%smouse(btn=%u%s)", off ? "+" : "",
                (unsigned)md.button_count, mcol ? ",collection" : "");
    }
    if (!off) copy_str(out, n, "hidkit 未识别的集合");
}

/*--------------------------------------------------------------------+
 * 描述符逐 item dump
 *--------------------------------------------------------------------*/

static const char *usage_page_name(uint16_t p)
{
    switch (p) {
    case 0x01: return "GenericDesktop";
    case 0x07: return "Keyboard";
    case 0x08: return "LED";
    case 0x09: return "Button";
    case 0x0C: return "Consumer";
    default:   return "Vendor/Other";
    }
}

static void dump_desc(const itf_t *t)
{
    printf("    描述符 %u 字节:\n", (unsigned)t->desc_len);
    const uint8_t *d = t->desc;
    uint32_t i = 0, len = t->desc_len;
    uint16_t g_page = 0;
    while (i < len) {
        uint8_t prefix = d[i++];
        uint8_t size = prefix & 0x03;
        if (size == 3) size = 4;
        uint8_t type = (prefix >> 2) & 0x03;
        uint8_t tag = prefix >> 4;
        if (prefix == 0xFE) { if (i < len) { uint8_t n = d[i++]; i += n; } continue; }
        if (i + size > len) break;

        uint32_t val = 0;
        for (uint8_t k = 0; k < size && k < 4; k++) val |= (uint32_t)d[i + k] << (8 * k);

        const char *ty = (type == 0) ? "Main" : (type == 1) ? "Global" : (type == 2) ? "Local" : "Res";
        printf("      %-6s ", ty);
        if (type == 1) {
            switch (tag) {
            case 0x0: printf("Usage Page  = 0x%02X (%s)", val, usage_page_name((uint16_t)val)); g_page = (uint16_t)val; break;
            case 0x1: case 0x2: {
                int32_t sv = (size == 1) ? (int8_t)val
                           : (size == 2) ? (int16_t)val : (int32_t)val;
                printf("%s = %d", tag == 0x1 ? "Logical Min" : "Logical Max", sv);
                break;
            }
            case 0x7: printf("Report Size = %u", val); break;
            case 0x8: printf("Report ID   = %u", val); break;
            case 0x9: printf("Report Count= %u", val); break;
            case 0xA: printf("Push"); break;
            case 0xB: printf("Pop"); break;
            default:  printf("Global tag=0x%X val=%u", tag, val); break;
            }
        } else if (type == 2) {
            switch (tag) {
            case 0x0: printf("Usage       = 0x%02X", val); break;
            case 0x1: printf("Usage Min   = 0x%02X", val); break;
            case 0x2: printf("Usage Max   = 0x%02X", val); break;
            default:  printf("Local tag=0x%X val=%u", tag, val); break;
            }
        } else if (type == 0) {
            switch (tag) {
            case 0x8: printf("Input       (page=0x%02X)", g_page); break;
            case 0x9: printf("Output"); break;
            case 0xB: printf("Feature"); break;
            case 0xA: printf("Collection  type=0x%X", val); break;
            case 0xC: printf("End Collection"); break;
            default:  printf("Main tag=0x%X val=%u", tag, val); break;
            }
        } else {
            printf("Reserved");
        }
        printf("\n");
        i += size;
    }

    /* hidkit 解析结论 */
    hid_nkro_desc_t nd;
    hid_mouse_desc_t md;
    memset(&nd, 0, sizeof(nd));
    memset(&md, 0, sizeof(md));
    if (hid_nkro_parse(&nd, t->desc, (uint16_t)t->desc_len)) {
        for (uint8_t s = 0; s < nd.num_spans; s++) {
            const hid_nkro_span_t *sp = &nd.spans[s];
            printf("      => NKRO span[%u]: report_id=%u bit_off=%u %s min=0x%02X count=%u\n",
                   s, sp->report_id, sp->bit_offset,
                   sp->bit_size == 1 ? "bitmap" : "array",
                   sp->usage_min, sp->count);
        }
    }
    if (hid_mouse_parse(&md, t->desc, (uint16_t)t->desc_len)) {
        printf("      => 鼠标: fields=%u buttons=%u", md.num_fields, md.button_count);
        if (md.idx_x != 0xFF) printf(" X@%u", md.fields[md.idx_x].bit_offset);
        if (md.idx_y != 0xFF) printf(" Y@%u", md.fields[md.idx_y].bit_offset);
        if (md.idx_wheel != 0xFF) printf(" Wheel@%u", md.fields[md.idx_wheel].bit_offset);
        printf(" (鼠标有多个集合时逐字段解析)\n");
    }
}

/*--------------------------------------------------------------------+
 * 事件出口（覆盖 hidkit 弱符号）
 *--------------------------------------------------------------------*/

static int g_nkey, g_nmouse_move, g_nmouse_btn, g_ngp;
static int32_t g_acc_dx, g_acc_dy, g_acc_wheel;
static struct timeval g_last_mouse;
static int g_mouse_ms = 100;

static const char *key_name(uint8_t u)
{
    static char b[16];
    if (u >= 0x04 && u <= 0x1D) { b[0] = (char)('a' + (u - 0x04)); b[1] = 0; return b; }
    if (u >= 0x1E && u <= 0x26) { b[0] = (char)('1' + (u - 0x1E)); b[1] = 0; return b; }
    switch (u) {
    case 0x27: return "0";
    case 0x28: return "Enter";
    case 0x29: return "Esc";
    case 0x2A: return "Backspace";
    case 0x2B: return "Tab";
    case 0x2C: return "Space";
    case 0x2D: return "-";
    case 0x2E: return "=";
    case 0x2F: return "[";
    case 0x30: return "]";
    case 0x31: return "\\";
    case 0x33: return ";";
    case 0x34: return "'";
    case 0x35: return "`";
    case 0x36: return ",";
    case 0x37: return ".";
    case 0x38: return "/";
    case 0x39: return "CapsLock";
    case 0x49: return "Insert";
    case 0x4A: return "Home";
    case 0x4B: return "PageUp";
    case 0x4C: return "Delete";
    case 0x4D: return "End";
    case 0x4E: return "PageDown";
    case 0x4F: return "Right";
    case 0x50: return "Left";
    case 0x51: return "Down";
    case 0x52: return "Up";
    case 0xE0: return "LCtrl";
    case 0xE1: return "LShift";
    case 0xE2: return "LAlt";
    case 0xE3: return "LMeta";
    case 0xE4: return "RCtrl";
    case 0xE5: return "RShift";
    case 0xE6: return "RAlt";
    case 0xE7: return "RMeta";
    default: break;
    }
    if (u >= 0x3A && u <= 0x45) { snprintf(b, sizeof(b), "F%d", u - 0x3A + 1); return b; }
    if (u >= 0x68 && u <= 0x73) { snprintf(b, sizeof(b), "F%d", u - 0x68 + 13); return b; }
    if (u >= 0xE8 && u <= 0xFB) { snprintf(b, sizeof(b), "Media(0x%02X)", u); return b; }
    snprintf(b, sizeof(b), "0x%02X", u);
    return b;
}

static const char *mouse_btn_name(uint8_t b)
{
    switch (b) {
    case 0: return "Left";
    case 1: return "Right";
    case 2: return "Middle";
    case 3: return "Back";
    case 4: return "Forward";
    default: return "Btn?";
    }
}

static void flush_mouse(void)
{
    if (g_acc_dx == 0 && g_acc_dy == 0 && g_acc_wheel == 0) return;
    printf("[EV] MOUSE move dx=%d dy=%d wheel=%d\n", g_acc_dx, g_acc_dy, g_acc_wheel);
    fflush(stdout);
    g_acc_dx = g_acc_dy = g_acc_wheel = 0;
    gettimeofday(&g_last_mouse, NULL);
}

void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
    uint16_t seg = HIDKIT_CODE_SEG(code);
    uint8_t  v = (uint8_t)(code & 0xFF);
    if (seg == HIDKIT_CODE_MOUSE) {
        g_nmouse_btn++;
        printf("[EV] slot=%d MOUSE %s %s\n", slot, mouse_btn_name(v), pressed ? "down" : "up");
    } else if (seg == HIDKIT_CODE_GAMEPAD) {
        g_nkey++;
        printf("[EV] slot=%d PAD btn=0x%02X %s\n", slot, v, pressed ? "down" : "up");
    } else {
        g_nkey++;
        printf("[EV] slot=%d KEY %-10s (0x%02X) %s\n", slot, key_name(v), v, pressed ? "down" : "up");
    }
    fflush(stdout);
}

void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    (void)slot;
    g_acc_dx += dx;
    g_acc_dy += dy;
    g_acc_wheel += wheel;
    g_nmouse_move++;

    if (g_mouse_ms <= 0) { flush_mouse(); return; }
    struct timeval now;
    gettimeofday(&now, NULL);
    long ms = (now.tv_sec - g_last_mouse.tv_sec) * 1000 +
              (now.tv_usec - g_last_mouse.tv_usec) / 1000;
    if (wheel != 0 || ms >= g_mouse_ms) flush_mouse();
}

void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt)
{
    g_ngp++;
    printf("[EV] slot=%d PAD ls=(%d,%d) rs=(%d,%d) lt=%d rt=%d\n",
           slot, ls_x, ls_y, rs_x, rs_y, lt, rt);
    fflush(stdout);
}

void hidkit_input_dropped(int8_t slot, uint16_t vid, uint16_t pid)
{
    printf("[EV] slot=%d DROPPED %04x:%04x\n", slot, vid, pid);
    fflush(stdout);
}

void hidkit_debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("[HKDBG] ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/*--------------------------------------------------------------------+
 * 选择与运行
 *--------------------------------------------------------------------*/

static void list_groups(void)
{
    if (g_ngrp == 0) {
        printf("没有找到 HID 设备（/dev/hidraw* 需要 root；若刚插上可稍等重试）\n");
        return;
    }
    for (int g = 0; g < g_ngrp; g++) {
        group_t *gr = &g_grp[g];
        printf("[%d] %04x:%04x  %-28s  %d 个 HID 接口\n",
               g, gr->vid, gr->pid, gr->name, gr->n);
        for (int k = 0; k < gr->n; k++) {
            itf_t *t = &g_itf[gr->idx[k]];
            char desc[192];
            describe(t, desc, sizeof(desc));
            printf("      %-14s iface~%d bInterfaceProtocol=%d desc=%s\n",
                   t->dev, t->input_idx, t->proto_real, t->desc_ok ? "ok" : "FAIL");
            printf("        分类: %s\n", desc);
        }
    }
}

static bool select_group(int *idx, int max_sel, int *n_sel,
                         const char *vidpid, const char *name, const char *index)
{
    *n_sel = 0;

    if (vidpid) {
        unsigned v = 0, p = 0;
        if (sscanf(vidpid, "%x:%x", &v, &p) != 2) return false;
        for (int g = 0; g < g_ngrp && *n_sel < max_sel; g++) {
            if (g_grp[g].vid == v && g_grp[g].pid == p) idx[(*n_sel)++] = g;
        }
        return *n_sel > 0;
    }
    if (name) {
        for (int g = 0; g < g_ngrp && *n_sel < max_sel; g++) {
            if (strcasestr(g_grp[g].name, name)) idx[(*n_sel)++] = g;
        }
        return *n_sel > 0;
    }
    if (index) {
        const char *s = index;
        while (*s && *n_sel < max_sel) {
            char *end = NULL;
            long v = strtol(s, &end, 10);
            if (end == s) break;
            if (v >= 0 && v < g_ngrp) idx[(*n_sel)++] = (int)v;
            if (*end != ',') break;
            s = end + 1;
        }
        return *n_sel > 0;
    }

    if (g_ngrp == 1) { idx[0] = 0; *n_sel = 1; return true; }

    if (!isatty(STDIN_FILENO)) return false;

    list_groups();
    printf("选择要监听的设备编号（多个用逗号分隔）: ");
    fflush(stdout);
    char buf[64];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    char *s = buf;
    while (*s && *n_sel < max_sel) {
        char *end = NULL;
        long v = strtol(s, &end, 10);
        if (end == s) break;
        if (v >= 0 && v < g_ngrp) idx[(*n_sel)++] = (int)v;
        if (*end != ',') break;
        s = end + 1;
    }
    return *n_sel > 0;
}

static void usage(const char *argv0)
{
    printf("用法: %s [选项]\n", argv0);
    printf("  --list             列出 HID 设备与 hidkit 分类\n");
    printf("  --vidpid V:P       选中 VID:PID\n");
    printf("  --index N[,N..]    选中 --list 里的第 N 组\n");
    printf("  --name SUBSTR      按名称子串选中\n");
    printf("  --proto N          mount proto（默认 0 = 描述符优先）\n");
    printf("  --seconds N        监听秒数（默认 0 = 直到 Ctrl-C）\n");
    printf("  --raw              打印原始报文\n");
    printf("  --dump-desc        打印描述符逐 item 解析\n");
    printf("  --mouse-ms N       鼠标位移聚合间隔毫秒（默认 100，0=每份）\n");
}

int main(int argc, char **argv)
{
    const char *vidpid = NULL, *name = NULL, *index = NULL;
    bool do_list = false, do_raw = false, do_dump = false;
    int proto = 0, seconds = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--list")) do_list = true;
        else if (!strcmp(argv[i], "--raw")) do_raw = true;
        else if (!strcmp(argv[i], "--dump-desc")) do_dump = true;
        else if (!strcmp(argv[i], "--vidpid") && i + 1 < argc) vidpid = argv[++i];
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--index") && i + 1 < argc) index = argv[++i];
        else if (!strcmp(argv[i], "--proto") && i + 1 < argc) proto = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mouse-ms") && i + 1 < argc) g_mouse_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "未知参数: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    enumerate();
    groupify();

    if (do_list) { list_groups(); return 0; }

    if (g_nitf == 0) {
        fprintf(stderr, "没有 HID 设备。请确认：设备已插好；用 sudo 运行（/dev/hidraw* 默认 root only）。\n");
        return 3;
    }
    if (g_ngrp == 0) {
        fprintf(stderr, "枚举到 %d 个 hidraw，但读不到 uevent。\n", g_nitf);
        return 3;
    }

    int sel[MAX_GROUPS], nsel = 0;
    if (!select_group(sel, MAX_GROUPS, &nsel, vidpid, name, index)) {
        fprintf(stderr, "无法确定目标设备，请用 --list 查看后用 --index/--vidpid/--name 指定。\n");
        list_groups();
        return 2;
    }

    hidkit_init();
    gettimeofday(&g_last_mouse, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int nfd = 0;
    for (int s = 0; s < nsel; s++) {
        group_t *gr = &g_grp[sel[s]];
        printf("==== 选中 [%d] %04x:%04x %s（%d 个接口）====\n",
               sel[s], gr->vid, gr->pid, gr->name, gr->n);
        for (int k = 0; k < gr->n; k++) {
            itf_t *t = &g_itf[gr->idx[k]];
            char desc[192];
            describe(t, desc, sizeof(desc));
            printf("  %s iface~%d proto=%d desc=%uB 分类: %s\n",
                   t->dev, t->input_idx, t->proto_real,
                   (unsigned)t->desc_len, desc);

            if (do_dump && t->desc_ok) dump_desc(t);

            if (!t->desc_ok) {
                fprintf(stderr, "  跳过：拿不到报告描述符（权限？）\n");
                continue;
            }
            t->fd = open(t->dev, O_RDWR | O_NONBLOCK);
            if (t->fd < 0) {
                fprintf(stderr, "  打开 %s 失败: %s\n", t->dev, strerror(errno));
                continue;
            }
            hidkit_dev_info_t info;
            memset(&info, 0, sizeof(info));
            info.vid = gr->vid;
            info.pid = gr->pid;
            info.dev_addr = (uint8_t)sel[s];
            info.itf = (uint8_t)(t->input_idx >= 0 ? t->input_idx : k);
            info.proto = (uint8_t)proto;
            info.report_desc = t->desc;
            info.report_desc_len = (uint16_t)t->desc_len;
            t->slot = hidkit_mount(&info);
            if (t->slot < 0) {
                fprintf(stderr, "  hidkit_mount 未接管（-1=不认识，-2=槽位满）: %d\n", t->slot);
                close(t->fd);
                t->fd = -1;
                continue;
            }
            printf("  → hidkit slot=%d 已接管\n", t->slot);
            nfd++;
        }
    }

    if (nfd == 0) {
        fprintf(stderr, "没有可监听的接口。\n");
        return 4;
    }

    printf("\n---- 开始监听（敲键盘 / 动鼠标；Linux 侧可同时用 evtest 对照）；%s ----\n",
           seconds > 0 ? "到时间自动结束" : "Ctrl-C 结束");
    fflush(stdout);

    struct timeval t0;
    gettimeofday(&t0, NULL);
    while (!g_stop) {
        if (seconds > 0) {
            struct timeval now;
            gettimeofday(&now, NULL);
            if (now.tv_sec - t0.tv_sec >= seconds) break;
        }
        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int i = 0; i < g_nitf; i++) {
            if (g_itf[i].fd < 0 || g_itf[i].slot < 0) continue;
            FD_SET(g_itf[i].fd, &rf);
            if (g_itf[i].fd > maxfd) maxfd = g_itf[i].fd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (r > 0) {
            for (int i = 0; i < g_nitf; i++) {
                if (g_itf[i].fd < 0 || g_itf[i].slot < 0) continue;
                if (!FD_ISSET(g_itf[i].fd, &rf)) continue;
                uint8_t buf[512];
                ssize_t n = read(g_itf[i].fd, buf, sizeof(buf));
                if (n <= 0) continue;
                if (do_raw) {
                    printf("[RX] slot=%d len=%d data=", g_itf[i].slot, (int)n);
                    for (ssize_t x = 0; x < n; x++) printf("%02X", buf[x]);
                    printf("\n");
                    fflush(stdout);
                }
                hidkit_report(g_itf[i].slot, buf, (uint16_t)n);
            }
        }
        flush_mouse();
    }
    flush_mouse();

    printf("\n---- 结束，补发松开并汇总 ----\n");
    for (int i = 0; i < g_nitf; i++) {
        if (g_itf[i].slot >= 0) {
            hidkit_umount(g_itf[i].slot);
            if (g_itf[i].fd >= 0) close(g_itf[i].fd);
        }
    }
    printf("SUMMARY key=%d mouse_btn=%d mouse_move=%d gamepad=%d\n",
           g_nkey, g_nmouse_btn, g_nmouse_move, g_ngp);
    return 0;
}
