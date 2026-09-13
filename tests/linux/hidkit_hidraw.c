/*
 * tests/linux/hidkit_hidraw.c
 *
 * 在 Linux 主机上用**真实 USB 枚举**验证 hidkit：
 *   1. 按 VID:PID 在 /sys/class/hidraw 里找到对应接口；
 *   2. ioctl(HIDIOCGRDESC) 取内核保存的报告描述符；
 *   3. hidkit_mount()（默认 proto=NONE，即完全靠描述符判定）；
 *   4. 读 /dev/hidrawN 的原始报文喂给 hidkit_report()，打印事件。
 *
 * 典型用法（和 USB gadget 配合，gadget 口回接到本机）：
 *   sudo ./hidkit_hidraw 3554:fa09 6            # 6 秒
 *   sudo ./hidkit_hidraw 0c45:8046 6 0          # 第 3 个参数可指定 proto
 *
 * 输出：`[DSC]` 描述符/分类、`[RX]` 原始报文、`[EV]` hidkit 事件、
 * 末尾 `SUMMARY key=.. mouse=.. gp=..`（供脚本断言）。
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <linux/hidraw.h>

#include "hidkit.h"

#define MAX_ITF 8

typedef struct {
    int      fd;
    int8_t   slot;
    int      itf;
    char     path[400];
} ent_t;

static ent_t g_ent[MAX_ITF];
static int   g_nent;
static int   g_key, g_mouse, g_gp;

/* ---- 覆盖 hidkit 弱符号出口：把事件打出来 ---- */
void hidkit_input_key(int8_t slot, uint16_t code, bool pressed)
{
    g_key++;
    const char *seg = (HIDKIT_CODE_SEG(code) == HIDKIT_CODE_MOUSE)   ? "MOUSE"
                    : (HIDKIT_CODE_SEG(code) == HIDKIT_CODE_GAMEPAD) ? "PAD" : "KEY";
    printf("[EV] slot=%d %s seg=0x%03X code=0x%02X %s\n", slot, seg,
           (unsigned)HIDKIT_CODE_SEG(code), (unsigned)(code & 0xFF),
           pressed ? "down" : "up");
    fflush(stdout);
}

void hidkit_input_mouse_abs(int8_t slot, int32_t dx, int32_t dy, int32_t wheel)
{
    g_mouse++;
    printf("[EV] slot=%d MOUSE dx=%d dy=%d wheel=%d\n", slot, dx, dy, wheel);
    fflush(stdout);
}

void hidkit_input_gamepad_abs(int8_t slot, int32_t ls_x, int32_t ls_y,
                              int32_t rs_x, int32_t rs_y, int32_t lt, int32_t rt)
{
    g_gp++;
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

/* uevent 里取 VID/PID 与 HID_PHYS 的 inputN 序号 */
static int read_uevent(const char *hidraw_path, uint16_t *vid, uint16_t *pid, int *input_idx)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/device/uevent", hidraw_path);
    FILE *f = fopen(p, "r");
    if (!f) return -1;

    char line[256];
    unsigned v = 0, pp = 0;
    int ii = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "HID_ID=", 7) == 0) {
            unsigned bus = 0;
            if (sscanf(line + 7, "%x:%x:%x", &bus, &v, &pp) == 3) { /* ok */ }
        } else {
            const char *q = strstr(line, "HID_PHYS=");
            if (q) {
                const char *r = strstr(q, "input");
                if (r) sscanf(r, "input%d", &ii);
            }
        }
    }
    fclose(f);
    *vid = (uint16_t)v;
    *pid = (uint16_t)pp;
    if (input_idx) *input_idx = ii;
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    if (x->itf != y->itf) return x->itf - y->itf;
    return strcmp(x->path, y->path);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <vid:pid> <seconds> [proto]\n", argv[0]);
        return 2;
    }
    unsigned vid = 0, pid = 0;
    if (sscanf(argv[1], "%x:%x", &vid, &pid) != 2) {
        fprintf(stderr, "bad vid:pid: %s\n", argv[1]);
        return 2;
    }
    int seconds = atoi(argv[2]);
    uint8_t proto = (argc > 3) ? (uint8_t)atoi(argv[3]) : HIDKIT_PROTO_NONE;

    hidkit_init();

    DIR *d = opendir("/sys/class/hidraw");
    if (!d) { perror("opendir /sys/class/hidraw"); return 2; }

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_nent < MAX_ITF) {
        if (strncmp(de->d_name, "hidraw", 6) != 0) continue;
        char sp[400];
        snprintf(sp, sizeof(sp), "/sys/class/hidraw/%s", de->d_name);

        uint16_t v = 0, p = 0;
        int ii = -1;
        if (read_uevent(sp, &v, &p, &ii) != 0) continue;
        if (v != vid || p != pid) continue;

        char dev[400];
        snprintf(dev, sizeof(dev), "/dev/%s", de->d_name);
        int fd = open(dev, O_RDWR | O_NONBLOCK);
        if (fd < 0) { perror(dev); continue; }

        struct hidraw_report_descriptor rpt;
        memset(&rpt, 0, sizeof(rpt));
        if (ioctl(fd, HIDIOCGRDESCSIZE, &rpt.size) < 0 ||
            ioctl(fd, HIDIOCGRDESC, &rpt) < 0) {
            perror("HIDIOCGRDESC");
            close(fd);
            continue;
        }

        ent_t *e = &g_ent[g_nent];
        e->fd = fd;
        e->itf = (ii >= 0) ? ii : g_nent;
        snprintf(e->path, sizeof(e->path), "%s", dev);

        hidkit_dev_info_t info;
        memset(&info, 0, sizeof(info));
        info.vid = v;
        info.pid = p;
        info.dev_addr = 0;
        info.itf = (uint8_t)e->itf;
        info.proto = proto;
        info.report_desc = rpt.value;
        info.report_desc_len = (uint16_t)rpt.size;

        e->slot = hidkit_mount(&info);
        printf("[DSC] %s itf=%d desc_len=%u proto=%u -> slot=%d gamepad=%d\n",
               e->path, e->itf, (unsigned)rpt.size, (unsigned)proto,
               (int)e->slot, (e->slot >= 0) ? (int)hidkit_is_gamepad(e->slot) : -1);
        fflush(stdout);
        g_nent++;
    }
    closedir(d);

    if (g_nent == 0) {
        fprintf(stderr, "no hidraw for %04x:%04x\n", vid, pid);
        return 3;
    }
    qsort(g_ent, (size_t)g_nent, sizeof(g_ent[0]), ent_cmp);

    printf("[READY] %d interface(s)\n", g_nent);
    fflush(stdout);

    struct timeval t0, now;
    gettimeofday(&t0, NULL);
    for (;;) {
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - t0.tv_sec);
        if (elapsed >= seconds) break;

        fd_set rf;
        FD_ZERO(&rf);
        int maxfd = -1;
        for (int i = 0; i < g_nent; i++) {
            FD_SET(g_ent[i].fd, &rf);
            if (g_ent[i].fd > maxfd) maxfd = g_ent[i].fd;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int r = select(maxfd + 1, &rf, NULL, NULL, &tv);
        if (r <= 0) continue;

        for (int i = 0; i < g_nent; i++) {
            if (!FD_ISSET(g_ent[i].fd, &rf)) continue;
            uint8_t buf[256];
            ssize_t n = read(g_ent[i].fd, buf, sizeof(buf));
            if (n <= 0) continue;
            printf("[RX] slot=%d len=%d data=", g_ent[i].slot, (int)n);
            for (ssize_t k = 0; k < n; k++) printf("%02X", buf[k]);
            printf("\n");
            fflush(stdout);
            hidkit_report(g_ent[i].slot, buf, (uint16_t)n);
        }
    }

    for (int i = 0; i < g_nent; i++) hidkit_umount(g_ent[i].slot);

    printf("SUMMARY key=%d mouse=%d gp=%d\n", g_key, g_mouse, g_gp);
    fflush(stdout);
    return 0;
}
