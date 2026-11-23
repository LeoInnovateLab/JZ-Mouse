/*
 * 京造 JZ M3 侧键映射 (macOS，2.4G 与蓝牙)
 * 后退侧键 -> Command+[      前进侧键 -> Command+]
 *
 * 核心修复：hidapi macOS 默认独占打开 (Seize)，调用 hid_darwin_set_open_exclusive(0)
 * 改为共享模式，设备继续归系统鼠标驱动管。
 */

#include <hidapi/hidapi.h>
#include <hidapi/hidapi_darwin.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <libproc.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#define VENDOR_ID          0x248A
#define TARGET_USAGE_PAGE  0xFF00
#define TARGET_USAGE       0

/* 2.4G (0x8373) 和蓝牙 (0x8266) 是两套设备，都监听 */
typedef struct { uint16_t pid; const char *name; } product_t;
static const product_t PRODUCTS[] = {
    { 0x8373, "2.4G 接收器" },
    { 0x8266, "蓝牙"        },
};
#define N_PRODUCTS ((int)(sizeof PRODUCTS / sizeof PRODUCTS[0]))

/* 兜底值：ReportDescriptor 读不出时使用（2.4G 实测值） */
#define DEFAULT_MOUSE_REPORT_ID  0x01
#define DEFAULT_BTN_OFFSET       1
#define BTN_BACK_MASK      0x08
#define BTN_FORWARD_MASK   0x10

#define kVK_ANSI_LeftBracket   33   /* '[' */
#define kVK_ANSI_RightBracket  30   /* ']' */
#define kVK_Command            55   /* 左 Command */

static int   g_probe   = 0;
static int   g_check   = 0;
static int   g_warned  = 0;

#define MAX_DEVS 8

typedef struct {
    hid_device *h;
    char        path[512];
    uint16_t    pid;
    uint8_t     report_id;
    int         btn_off;
    int         btn_bits;
    uint8_t     last_btn;
} jz_dev;

static jz_dev g_devs[MAX_DEVS];
static int    g_ndevs = 0;

/* 非鼠标接口的 path 黑名单（复合设备的键盘/多媒体），环形缓冲 */
#define MAX_SKIP 32
static char g_skip[MAX_SKIP][512];
static int  g_nskip = 0, g_skip_next = 0;

static const char *pid_name(uint16_t pid)
{
    for (int i = 0; i < N_PRODUCTS; i++)
        if (PRODUCTS[i].pid == pid)
            return PRODUCTS[i].name;
    return "未知连接";
}

/* ------------------------------------------------------------------ */

static void send_cmd_shortcut(CGKeyCode keycode)
{
    CGEventSourceRef src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);

    CGEventRef ev[4];
    ev[0] = CGEventCreateKeyboardEvent(src, kVK_Command,        true);
    ev[1] = CGEventCreateKeyboardEvent(src, keycode,            true);
    ev[2] = CGEventCreateKeyboardEvent(src, keycode,            false);
    ev[3] = CGEventCreateKeyboardEvent(src, kVK_Command,        false);
    if (!ev[0] || !ev[1] || !ev[2] || !ev[3]) {
        for (int i = 0; i < 4; i++) if (ev[i]) CFRelease(ev[i]);
        if (src) CFRelease(src);
        return;
    }

    CGEventSetType(ev[0], kCGEventKeyDown);
    CGEventSetType(ev[1], kCGEventKeyDown);
    CGEventSetType(ev[2], kCGEventKeyUp);
    CGEventSetType(ev[3], kCGEventKeyUp);
    CGEventSetFlags(ev[0], kCGEventFlagMaskCommand);
    CGEventSetFlags(ev[1], kCGEventFlagMaskCommand);
    CGEventSetFlags(ev[2], kCGEventFlagMaskCommand);
    CGEventSetFlags(ev[3], 0);

    for (int i = 0; i < 4; i++)
        CGEventPost(kCGHIDEventTap, ev[i]);

    for (int i = 0; i < 4; i++)
        CFRelease(ev[i]);
    if (src)
        CFRelease(src);
}

/*
 * 从 ReportDescriptor 解析鼠标按键位图的位置。
 * 2.4G 和蓝牙的描述符不同（Report ID、字段排布），不能硬编码。
 * 要求：字段必须在 Mouse collection 内，Usage Page=Button，位数>=3，
 * 避免把复合设备的键盘接口误认为鼠标（键盘 buf[1] 是修饰键位图）。
 *
 * 返回：>0 按键位数（是鼠标）；0 无鼠标按键位图（键盘/多媒体）；-1 读不到描述符
 */
static int parse_report_descriptor(const unsigned char *desc, int n,
                                   uint8_t *out_id, int *out_off)
{
    *out_id  = DEFAULT_MOUSE_REPORT_ID;
    *out_off = DEFAULT_BTN_OFFSET;

    int cur_id = 0, bitpos = 0;
    int upage = -1, rsize = 0, rcount = 0;
    int found_id = -1, found_bit = -1, found_bits = 0;
    int usage = -1;
    int depth = 0, mouse_depth = -1;

    for (int i = 0; i < n; ) {
        unsigned char b = desc[i];
        if ((b & 0xFE) == 0xFE) { i += 2; continue; }

        int  len  = b & 0x03;  if (len == 3) len = 4;
        if (i + 1 + len > n) break;

        int  type = b & 0x0C;
        int  tag  = b & 0xF0;
        long val  = 0;
        for (int k = 0; k < len; k++)
            val |= (long)desc[i + 1 + k] << (8 * k);
        i += 1 + len;

        if (type == 0x04) {
            switch (tag) {
            case 0x00: upage  = (int)val; break;
            case 0x70: rsize  = (int)val; break;
            case 0x90: rcount = (int)val; break;
            case 0x80:
                if ((int)val != cur_id) { cur_id = (int)val; bitpos = 0; }
                break;
            default: break;
            }
        } else if (type == 0x08 && tag == 0x00) {
            usage = (int)val;
        } else if (type == 0x00 && tag == 0xA0) {
            depth++;
            if (upage == 0x01 && usage == 0x02)
                mouse_depth = depth;
            usage = -1;
        } else if (type == 0x00 && tag == 0xC0) {
            if (mouse_depth == depth)
                mouse_depth = -1;
            if (depth > 0)
                depth--;
            usage = -1;
        } else if (type == 0x00 && tag == 0x80) {
            int bits = rsize * rcount;
            if (found_id < 0 && upage == 0x09 && rsize == 1 &&
                bits >= 3 && mouse_depth > 0) {
                found_id   = cur_id;
                found_bit  = bitpos;
                found_bits = bits;
            }
            bitpos += bits;
            usage = -1;
        }
    }

    if (found_id < 0)
        return 0;

    *out_id  = (uint8_t)found_id;
    *out_off = 1 + found_bit / 8;
    return found_bits;
}

static int detect_mouse_report(hid_device *h, uint8_t *out_id, int *out_off)
{
    unsigned char desc[4096];
    int n = hid_get_report_descriptor(h, desc, sizeof desc);
    if (n <= 0)
        return -1;
    return parse_report_descriptor(desc, n, out_id, out_off);
}

/*
 * 枚举并打开所有在线的 JZ M3（2.4G + 蓝牙）。
 * 同一 IOHIDDevice 的多个顶层集合 path 相同，按 path 去重；
 * 复合设备的不同接口 path 不同，靠描述符过滤非鼠标接口。
 */
static int open_all_jzm3(void)
{
    struct hid_device_info *devs = hid_enumerate(VENDOR_ID, 0x0000);
    int opened = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (struct hid_device_info *cur = devs; cur; cur = cur->next) {
            int known = 0;
            for (int i = 0; i < N_PRODUCTS; i++)
                if (cur->product_id == PRODUCTS[i].pid) { known = 1; break; }
            if (!known)
                continue;

            int vendor_coll = (cur->usage_page == TARGET_USAGE_PAGE &&
                               cur->usage == TARGET_USAGE);
            if (pass == 0 && !vendor_coll)
                continue;

            const char *spath = (const char *)cur->path;

            int dup = 0;
            for (int i = 0; i < g_ndevs; i++)
                if (!strcmp(g_devs[i].path, spath)) { dup = 1; break; }
            for (int i = 0; i < g_nskip && !dup; i++)
                if (!strcmp(g_skip[i], spath)) dup = 1;
            if (dup)
                continue;

            if (g_ndevs >= MAX_DEVS) {
                fprintf(stderr, "⚠️  已达设备上限 %d，忽略后面的\n", MAX_DEVS);
                goto done;
            }

            hid_device *h = hid_open_path(cur->path);
            if (!h)
                goto done;

            uint8_t rid;
            int     off;
            int     bits = detect_mouse_report(h, &rid, &off);

            if (bits == 0) {
                snprintf(g_skip[g_skip_next], sizeof g_skip[0], "%s", spath);
                g_skip_next = (g_skip_next + 1) % MAX_SKIP;
                if (g_nskip < MAX_SKIP)
                    g_nskip++;
                hid_close(h);
                if (g_probe)
                    printf("  ↳ 忽略非鼠标接口：%s\n", spath);
                continue;
            }
            if (bits < 0)
                fprintf(stderr, "⚠️  读不到 ReportDescriptor，按 2.4G 格式兜底\n");

            jz_dev *d = &g_devs[g_ndevs++];
            memset(d, 0, sizeof *d);
            d->h         = h;
            d->pid       = cur->product_id;
            d->report_id = rid;
            d->btn_off   = off;
            d->btn_bits  = bits > 0 ? bits : 5;
            snprintf(d->path, sizeof d->path, "%s", spath);
            hid_set_nonblocking(h, 1);

            printf("✅ 已连接 [%s pid=0x%04X] 鼠标报告 ID=0x%02X 按键字节=buf[%d]"
                   " 声明 %d 个按键位\n",
                   pid_name(d->pid), d->pid, d->report_id, d->btn_off, d->btn_bits);
            opened++;
        }
    }

done:
    hid_free_enumeration(devs);
    return opened;
}

static void close_dev(int idx)
{
    if (idx < 0 || idx >= g_ndevs)
        return;
    printf("⚠️  设备 [%s pid=0x%04X] 已断开\n",
           pid_name(g_devs[idx].pid), g_devs[idx].pid);
    hid_close(g_devs[idx].h);
    g_devs[idx] = g_devs[g_ndevs - 1];
    g_ndevs--;
}

static void close_all_devs(void)
{
    while (g_ndevs > 0) {
        hid_close(g_devs[g_ndevs - 1].h);
        g_ndevs--;
    }
}

static void handle_report(jz_dev *d, const unsigned char *buf, int len)
{
    int     is_mouse = (len > d->btn_off && buf[0] == d->report_id);
    uint8_t btn      = is_mouse ? buf[d->btn_off] : 0;

    if (g_probe && (!is_mouse || btn != d->last_btn)) {
        printf("  [%s] report[%2d]:", pid_name(d->pid), len);
        for (int i = 0; i < len && i < 8; i++)
            printf(" %02X", buf[i]);
        printf("%s\n", is_mouse ? "" : "   <- 非鼠标报告");
    }

    if (!is_mouse || btn == d->last_btn)
        return;

    if ((btn & BTN_BACK_MASK) && !(d->last_btn & BTN_BACK_MASK)) {
        send_cmd_shortcut(kVK_ANSI_LeftBracket);
        printf("后侧键 -> Cmd+[\n");
    }
    if ((btn & BTN_FORWARD_MASK) && !(d->last_btn & BTN_FORWARD_MASK)) {
        send_cmd_shortcut(kVK_ANSI_RightBracket);
        printf("前侧键 -> Cmd+]\n");
    }
    d->last_btn = btn;
}

static void print_last_error(const char *stage)
{
    const wchar_t *err = hid_error(NULL);
    char buf[512] = {0};

    if (err)
        wcstombs(buf, err, sizeof(buf) - 1);

    fprintf(stderr, "❌ %s：%s\n", stage, buf[0] ? buf : "(无错误信息)");

    if (strstr(buf, "not permitted") || strstr(buf, "0xE00002E2") ||
        strstr(buf, "0xe00002e2")) {
        if (!g_warned) {
            g_warned = 1;
            fprintf(stderr,
                "   这是「输入监控」权限缺失（该设备 RequiresTCCAuthorization=Yes）。\n"
                "   TCC 授权记在责任进程上：\n"
                "     • 在 iTerm/Terminal 里运行 -> 系统设置 > 隐私与安全性 > 输入监控，\n"
                "       勾选 iTerm/Terminal，然后**完全退出终端再重开**（授权不会热更新）；\n"
                "     • 想常驻且不依赖终端 -> ./install.sh 打成 .app + LaunchAgent，\n"
                "       再给「隐私与安全性 > 输入监控」里加上那个 .app。\n"
                "   另外：本程序不需要「辅助功能」权限，之前那条授权记录可以删掉。\n");
        }
    }
}

/* 打印 TCC 授权身份和状态 */
static IOHIDAccessType dump_auth_state(const char *tag)
{
    static const char *state_names[] = {
        "Granted(已授权)", "Denied(已拒绝)", "Unknown(未申请)"
    };
    IOHIDAccessType t = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);

    CFBundleRef b = CFBundleGetMainBundle();
    const char  *bid = "(不属任何 bundle)";
    char exe[1024]   = "(?)";
    char parent[256] = "(?)";

    if (b) {
        CFStringRef i = CFBundleGetIdentifier(b);
        static char idbuf[128];
        if (i && CFStringGetCString(i, idbuf, sizeof idbuf, kCFStringEncodingUTF8))
            bid = idbuf;
        CFURLRef u = CFBundleCopyExecutableURL(b);
        if (u) {
            CFURLGetFileSystemRepresentation(u, true, (UInt8 *)exe, sizeof exe);
            CFRelease(u);
        }
    }

    pid_t pp = getppid();
    if (proc_name(pp, parent, sizeof parent) <= 0)
        snprintf(parent, sizeof parent, "(未知)");

    printf("  身份[%s]: 父进程=%s(%d)  bundle_id=%s\n"
           "            exe=%s\n"
           "            ListenEvent=%s\n",
           tag, parent, (int)pp, bid, exe, t <= 2 ? state_names[t] : "?");
    fflush(stdout);
    return t;
}

/* 向 TCC 申请输入监控授权（后台进程必须主动申请，否则列表里不会出现） */
static void request_input_monitoring(void)
{
    if (dump_auth_state("startup") == kIOHIDAccessTypeGranted) {
        printf("「输入监控」已授权\n");
        return;
    }

    printf("正在请求「输入监控」授权……\n");

    if (IOHIDRequestAccess(kIOHIDRequestTypeListenEvent)) {
        printf("✅ 已获授权\n");
        return;
    }

    printf("⚠️  暂未获授权。现在本程序应该已经出现在「输入监控」列表里了：\n"
           "   系统设置 > 隐私与安全性 > 输入监控 -> 打开它的开关；\n"
           "   开关一开，守护进程最多 10 秒后自己就通（也可立刻重启：\n"
           "   launchctl kickstart -k gui/%d/com.jzm3.side-buttons）\n",
           (int)geteuid());
}

static void usage(const char *argv0)
{
    printf("京造 JZ M3 侧键映射守护进程 (macOS, 2.4G + 蓝牙)\n\n"
           "用法: %s [选项]\n\n"
           "  --check   只测一次能否以共享方式打开设备，然后退出（排查权限）\n"
           "  --auth-probe  只打印 TCC 认的是哪个身份、「输入监控」状态如何\n"
           "  --probe   同时打印收到的原始 HID 报告，用于核对报文格式\n"
           "  -h, --help  显示本帮助\n\n"
           "常驻运行：./install.sh          卸载：./install.sh --remove\n\n"
           "所需权限：系统设置 > 隐私与安全性 > 输入监控\n"
           "（授权对象是\"责任进程\"：终端里跑就是终端，LaunchAgent 跑就是那个 .app）\n"
           "本程序以共享方式(kIOHIDOptionsTypeNone)打开设备，不会抢占系统鼠标驱动；\n"
           "同一时间只允许一个实例，避免一次侧键触发两遍快捷键。\n",
           argv0);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *argv0 = argv[0];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe"))          g_probe = 1;
        else if (!strcmp(argv[i], "--check"))     g_check = 1;
        else if (!strcmp(argv[i], "--auth-probe")) {
            if (freopen("/tmp/jzm3_auth_probe.log", "a", stdout))
                fprintf(stderr, "（结果也写进了 /tmp/jzm3_auth_probe.log）\n");
            dump_auth_state("auth-probe");
            return 0;
        }
        else if (!strcmp(argv[i], "--auth-request")) {
            if (freopen("/tmp/jzm3_auth_probe.log", "a", stdout))
                fprintf(stderr, "（结果写进了 /tmp/jzm3_auth_probe.log）\n");
            request_input_monitoring();
            return 0;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv0);
            return 0;
        } else {
            fprintf(stderr, "未知参数：%s\n\n", argv[i]);
            usage(argv0);
            return 2;
        }
    }

    /* 单实例锁 */
    if (!g_check) {
        char lock_path[128];
        snprintf(lock_path, sizeof lock_path,
                 "/tmp/com.jzm3.side-buttons.%d.lock", (int)geteuid());
        int lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock_fd < 0) {
            fprintf(stderr, "❌ 无法创建锁文件 %s: %s\n", lock_path, strerror(errno));
        } else if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr,
                    "❌ 已有一个实例在运行（锁：%s）。\n"
                    "   同时跑两份会让一次侧键触发两遍快捷键，请先退掉另一份：\n"
                    "     pkill -x jzm3_side_buttons\n"
                    "   若它是后台服务：\n"
                    "     launchctl bootout gui/%d/com.jzm3.side-buttons\n",
                    lock_path, (int)geteuid());
            close(lock_fd);
            return 3;
        }
        /* 故意不 close(lock_fd)：进程退出时锁自动释放 */
    }

    if (hid_init() != 0) {
        print_last_error("初始化 hidapi 失败");
        return 1;
    }

    hid_darwin_set_open_exclusive(0);
    if (hid_darwin_get_open_exclusive() != 0) {
        fprintf(stderr, "❌ 无法关闭独占打开，请确认 hidapi 0.14.0+\n");
        hid_exit();
        return 1;
    }

    request_input_monitoring();

    printf("build: %s %s   打开方式：共享\n", __DATE__, __TIME__);
    printf("正在监听 JZ M3 (VID:0x%04X)，共享方式打开（不抢占鼠标）：", VENDOR_ID);
    for (int i = 0; i < N_PRODUCTS; i++)
        printf("%s=0x%04X ", PRODUCTS[i].name, PRODUCTS[i].pid);
    printf("\n");

    unsigned char *buf = calloc(1, 64);
    if (!buf) {
        hid_exit();
        return 1;
    }

    int  backoff = 2;
    long total_reports = 0, ticked = 0;
    int  idle_rounds = 0, silent_hinted = 0;
    time_t last_scan = time(NULL);   /* 定期补扫，接住中途连上的另一种连接模式 */

    while (1) {
        int added = open_all_jzm3();

        if (g_ndevs == 0) {
            if (!added)
                print_last_error("打开设备失败");
            if (g_check) {
                free(buf);
                hid_exit();
                return 1;
            }
            sleep(backoff);
            if (backoff < 10)
                backoff += 2;
            continue;
        }
        backoff = 2;

        /* 双保险：万一又被独占打开，立刻关掉 */
        for (int i = 0; i < g_ndevs; i++) {
            if (hid_darwin_is_device_open_exclusive(g_devs[i].h)) {
                fprintf(stderr, "⚠️  [%s] 仍以独占方式打开，正在关闭\n",
                        pid_name(g_devs[i].pid));
                hid_darwin_set_open_exclusive(0);
                close_dev(i);
                i = -1;
            }
        }
        if (g_ndevs == 0)
            continue;

        if (g_check) {
            printf("✅ 自检通过\n");
            close_all_devs();
            free(buf);
            hid_exit();
            return 0;
        }

        while (g_ndevs > 0) {
            int got = 0;

            for (int i = 0; i < g_ndevs; ) {
                int n = hid_read(g_devs[i].h, buf, 64);
                if (n < 0) {
                    close_dev(i);
                    last_scan = 0;
                    continue;
                }
                if (n > 0) {
                    got = 1;
                    total_reports++;
                    handle_report(&g_devs[i], buf, n);
                }
                i++;
            }
            if (g_ndevs == 0)
                break;

            /* 定期补扫，接住中途连上的另一种连接模式 */
            time_t now = time(NULL);
            if (now - last_scan >= 3) {
                last_scan = now;
                open_all_jzm3();
            }

            if (got) {
                idle_rounds = 0;
                continue;
            }
            usleep(4000);

            if (g_probe && ++idle_rounds >= 500) {
                idle_rounds = 0;
                if (total_reports != ticked) {
                    printf("  （2 秒内共收到 %ld 份报告）\n", total_reports - ticked);
                    ticked = total_reports;
                    silent_hinted = 0;
                } else if (!silent_hinted) {
                    silent_hinted = 1;
                    printf("  （静默中——请移动鼠标或按侧键）\n");
                }
            }
        }

        sleep(2);
    }

    free(buf);
    hid_exit();
    return 0;
}
