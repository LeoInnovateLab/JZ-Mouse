/*
 * 京造 JZ M3 侧键映射 (macOS，2.4G 与蓝牙两种连接)
 * 后退侧键 -> Command+[      前进侧键 -> Command+]
 *
 * ============================ 根因（实测） ============================
 * 旧版“侧键能用、但鼠标不能动也不能点”的原因只有一行代码：
 *
 *   hidapi 在 macOS 上默认以**独占方式**打开设备。
 *   其源码 mac/hid.c 的 hid_init() 里有（注释原文 "Backward compatibility"）：
 *       hid_darwin_set_open_exclusive(1);
 *   于是 hid_open_path() 内部执行的是
 *       IOHIDDeviceOpen(handle, kIOHIDOptionsTypeSeizeDevice)
 *   按 Apple TN2187，Seize 就是把设备从 HID 事件系统上摘下来交给
 *   当前客户端 —— 光标移动、左右键、滚轮全部停止上报，只有本进程
 *   还能看到原始报告，所以表现为“只有侧键生效”。
 *   实测确认：hid_darwin_get_open_exclusive() 返回 1。
 *
 *   另外，旧注释里“只打开 0xFF00 厂商端口就能绕开鼠标端口”也不成立：
 *   接口 0 的 6 个顶层集合（鼠标 1/2、键盘 1/1、多媒体 12/1、系统控制
 *   1/80、厂商 FF00、厂商 FF01）属于**同一个 IOHIDDevice**，hidapi 给
 *   它们列出的 path 完全相同（DevSrvsID:4295565612，一个 path = 一个
 *   USB 接口）。所以无论匹配哪个集合，open 的都是同一个设备。
 *
 * 修复 = 一行：hid_init() 之后调用 hid_darwin_set_open_exclusive(0)，
 * 以共享方式打开。设备继续归 macOS 鼠标驱动管，原始报告同时送给本进程。
 *
 * ================== 为什么不能用 CGEventTap（别踩坑） ==================
 * 解析接口 0 的 ReportDescriptor：
 *     Report ID 1 (鼠标): Usage Page Button, Usage Min 1, Usage Max 3,
 *                         Report Size 1, Report Count 5   <- 5 个按键位
 *     Report ID 5 (0xFF00): 7 字节 Telink 配置通道，不含任何按键
 * 侧键确实是鼠标报告里的第 4、5 个按键位（掩码 0x08 / 0x10）。
 * 但 macOS 只按声明的 Usage Min..Max 解析出 3 个键：
 *     ioreg 中该设备 AppleUserHIDEventService 的 "HIDPointerButtonCount" = 3
 * 也就是说第 4、5 位被当成 padding，**永远不会变成 otherMouseDown 事件**，
 * 事件钩子方案在这只鼠标（2.4G）上收不到侧键，只能读原始 HID 报告。
 *
 * ============================ 连接模式 ============================
 * 同一只 M3 在系统里是两套设备，PID 不同：
 *     0x8373 = 2.4G 接收器      0x8266 = 蓝牙（多彩方案）
 * 所以枚举只按 VID 走、PID 用表匹配，2.4G 与蓝牙都听；切换连接方式时旧句柄
 * 读回 -1 会自动断开，外层循环约 2 秒内重新枚举接上，不用重启进程。
 * 蓝牙那套 ReportDescriptor 不保证和 2.4G 一致，因此“鼠标报告是哪个 ID、
 * 按键位图在第几个字节”改成从描述符里现学（detect_mouse_report），学不到
 * 才退回 2.4G 的实测值：Report ID 0x01、buf[1]。
 *
 * ============================ 权限说明 ============================
 * 该设备 ioreg 属性 "RequiresTCCAuthorization" = Yes，因此打开它需要
 * 「输入监控」授权（macOS 15 起强制），否则 IOHIDDeviceOpen 返回
 * 0xE00002E2 (not permitted)。
 * 注意 TCC 记的是“责任进程”：在 iTerm 里跑就是 iTerm 需要授权，
 * 给二进制本身授权无效。所以要常驻/全局生效得用 ./install.sh —— 它把
 * 本程序包成 JZMSideButtons.app 交给 LaunchAgent 启动，授权对象才是它自己。
 * ==================================================================
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
#define TARGET_USAGE_PAGE  0xFF00   /* 厂商私有 Usage Page */
#define TARGET_USAGE       0

/*
 * 一只 JZ M3 在不同连接方式下是两个不同的 PID，所以按“多设备”处理：
 * 2.4G 与蓝牙谁在就听谁，两个都在就都听（同一只鼠标不会同时发两份侧键，
 * 但换模式时不用重启进程）。
 */
typedef struct { uint16_t pid; const char *name; } product_t;
static const product_t PRODUCTS[] = {
    { 0x8373, "2.4G 接收器" },
    { 0x8266, "蓝牙"        },
};
#define N_PRODUCTS ((int)(sizeof PRODUCTS / sizeof PRODUCTS[0]))

/* 兜底值：ReportDescriptor 里读不出按键字段时才用（2.4G 实测就是这个） */
#define DEFAULT_MOUSE_REPORT_ID  0x01
#define DEFAULT_BTN_OFFSET       1      /* buf[0]=Report ID，buf[1]=按键位图 */
#define BTN_BACK_MASK      0x08     /* 后侧键 = 第 4 键 */
#define BTN_FORWARD_MASK   0x10     /* 前侧键 = 第 5 键 */

#define kVK_ANSI_LeftBracket   33   /* '[' */
#define kVK_ANSI_RightBracket  30   /* ']' */
#define kVK_Command            55   /* 左 Command */

static int   g_probe   = 0;   /* 打印原始报告，用于核对报文格式 */
static int   g_check   = 0;   /* 只做一次连接自检后退出 */
static int   g_warned  = 0;   /* 权限提示只打一次 */

#define MAX_DEVS 8

typedef struct {
    hid_device *h;
    char        path[512];    /* 一个 path = 一个 IOHIDDevice，用来去重 */
    uint16_t    pid;
    uint8_t     report_id;    /* 鼠标报告 ID（从 ReportDescriptor 解析出来） */
    int         btn_off;      /* 按键位图在原始报文里的下标 */
    int         btn_bits;     /* 描述符声明了几个按键位 */
    uint8_t     last_btn;
} jz_dev;

static jz_dev g_devs[MAX_DEVS];
static int    g_ndevs = 0;

/* 确认“不是鼠标”的 path（复合设备的键盘/多媒体接口），下次不再重复开。
 * 不收这条黑名单的话，一次按键会被多个句柄各报一遍。 */
/*
 * 确认“不是鼠标”的 path 记在这里，下次扫描直接跳过。
 * 每插拔一次 / 切一次连接模式，path 里的 IOService 编号就换一个，所以容量给足
 * 并用先到先得的环形覆盖：一旦装满，最旧的让位给新的，缓存不会失效或堵死。
 */
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

    /* HID 系统态 + HID 级投递：修饰位会被 Chromium/Electron/Java 等 app 认到；
     * kCGSessionEventTap + Private 源在不少 app 里 ⌘ 位会被吞掉 */
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
 * 从 ReportDescriptor 里认出“鼠标按键位图在哪一个报告的哪一个字节”。
 *
 * 为什么不能写死：2.4G（0x8373）和蓝牙（0x8266）是两套描述符，Report ID 和
 * 字段排布没有保证一致。写死 0x01/第 2 字节在 2.4G 上是对的，换蓝牙就可能变成
 * “侧键再也不响应”，而且现象和权限问题长得一样，很难查。
 *
 * 做法：按 HID 短 item 走一遍，遇到 Global Report ID 就换当前报告并清零位偏移；
 * 遇到 Main Input（type=0x00, tag=0x80）就累加它占的 bit 数。第一个
 * Usage Page(Button) + Report Size=1 的 Input 字段就是要盯的按键位图。
 * 再加两道锁：该字段必须落在 Collection(Application=Mouse) 里，且位数 >= 3，
 * 免得把复合设备的键盘/多媒体接口错认成鼠标（真出过：键盘报告第 2 字节是
 * 修饰键位图，0x08 = Left Command，一按 Command 就被当成侧键）。
 *
 * 返回值：>0 = 声明的按键位数（说明这是个鼠标）；0 = 描述符里没有鼠标按键位图
 * （复合设备的键盘/多媒体接口，必须剔除）；-1 = 读不到描述符，只能按兜底值。
 */
static int parse_report_descriptor(const unsigned char *desc, int n,
                                   uint8_t *out_id, int *out_off)
{
    *out_id  = DEFAULT_MOUSE_REPORT_ID;
    *out_off = DEFAULT_BTN_OFFSET;

    int cur_id = 0, bitpos = 0;
    int upage = -1, rsize = 0, rcount = 0;
    int found_id = -1, found_bit = -1, found_bits = 0;
    int usage = -1;                     /* Local Usage，喂给下一个 Collection */
    int depth = 0, mouse_depth = -1;

    for (int i = 0; i < n; ) {
        unsigned char b = desc[i];
        if ((b & 0xFE) == 0xFE) { i += 2; continue; }   /* Long item，跳过 */

        int  len  = b & 0x03;  if (len == 3) len = 4;
        if (i + 1 + len > n) break;

        int  type = b & 0x0C;                 /* 00=Main 04=Global 08=Local */
        int  tag  = b & 0xF0;
        long val  = 0;
        for (int k = 0; k < len; k++)
            val |= (long)desc[i + 1 + k] << (8 * k);
        i += 1 + len;

        if (type == 0x04) {                   /* Global */
            switch (tag) {
            case 0x00: upage  = (int)val; break;                     /* Usage Page */
            case 0x70: rsize  = (int)val; break;                     /* Report Size */
            case 0x90: rcount = (int)val; break;                     /* Report Count */
            case 0x80:                                                    /* Report ID */
                if ((int)val != cur_id) { cur_id = (int)val; bitpos = 0; }
                break;
            default: break;
            }
        } else if (type == 0x08 && tag == 0x00) {   /* Local: Usage（头字节是 0x08|长度） */
            usage = (int)val;
        } else if (type == 0x00 && tag == 0xA0) {   /* Main: Collection */
            depth++;
            if (upage == 0x01 && usage == 0x02)     /* Generic Desktop / Mouse */
                mouse_depth = depth;
            usage = -1;
        } else if (type == 0x00 && tag == 0xC0) {   /* Main: End Collection */
            if (mouse_depth == depth)
                mouse_depth = -1;
            if (depth > 0)
                depth--;
            usage = -1;                  /* Local 项只作用于紧跟的那个 Main 项 */
        } else if (type == 0x00 && tag == 0x80) {   /* Main: Input */
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
        return 0;                            /* 这个设备根本不含按键位图 */

    /* hidapi 把 Report ID 放在 buf[0]，所以字段偏移要再加 1 个字节 */
    *out_id  = (uint8_t)found_id;
    *out_off = 1 + found_bit / 8;
    return found_bits;
}

/* 打开设备后取描述符，交给纯函数 parse_report_descriptor 解析 */
static int detect_mouse_report(hid_device *h, uint8_t *out_id, int *out_off)
{
    unsigned char desc[4096];
    int n = hid_get_report_descriptor(h, desc, sizeof desc);

    if (n <= 0)
        return -1;                       /* 老 hidapi / 读不到，用兜底值 */
    return parse_report_descriptor(desc, n, out_id, out_off);
}

/*
 * 枚举并打开所有在线的 JZ M3（2.4G + 蓝牙），已打开的跳过；返回本轮新增数。
 *
 * 关键点一：同一个 IOHIDDevice 的每个顶层集合都会被 hidapi 列成一条记录，
 * 但 path 相同，所以按 path 去重（否则一只鼠标开成好几份、一次侧键触发好几遍）。
 * 关键点二：复合设备的每个 USB 接口是**另一个** IOHIDDevice（JZ M3 的接口 1
 * 是键盘），path 不同、去重拦不住，只能靠“描述符里必须有 Mouse 的按键位图”
 * 把它筛掉 —— 见 parse_report_descriptor 的说明。
 */
static int open_all_jzm3(void)
{
    struct hid_device_info *devs = hid_enumerate(VENDOR_ID, 0x0000);
    int opened = 0;

    /* 两遍扫描：第一遍只开带 0xFF00 厂商集合的设备（保持原来的语义），
     * 第二遍再收留其它集合 —— 蓝牙模式未必声明 0xFF00，而反正同一个 path
     * 打开的是同一个 IOHIDDevice，选哪个集合不影响结果。 */
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
                if (!strcmp(g_skip[i], spath)) dup = 1;   /* 已确认不是鼠标 */
            if (dup)
                continue;

            if (g_ndevs >= MAX_DEVS) {
                fprintf(stderr, "⚠️  已达设备上限 %d，忽略后面的\n", MAX_DEVS);
                goto done;
            }

            hid_device *h = hid_open_path(cur->path);
            if (!h)
                goto done;                 /* 多半是权限，交给调用方报错重试 */

            uint8_t rid;
            int     off;
            int     bits = detect_mouse_report(h, &rid, &off);

            if (bits == 0) {
                /* 复合设备的另一个接口（键盘/多媒体）：坚决不收。收了不仅一次
                 * 侧键报两遍，键盘报告第 2 字节的修饰键位图（0x08=Left Command）
                 * 还会被当成侧键按下。 */
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
                fprintf(stderr, "⚠️  读不到 ReportDescriptor，按 2.4G 的实测格式兜底\n");

            jz_dev *d = &g_devs[g_ndevs++];
            memset(d, 0, sizeof *d);
            d->h         = h;
            d->pid       = cur->product_id;
            d->report_id = rid;
            d->btn_off   = off;
            d->btn_bits  = bits > 0 ? bits : 5;
            snprintf(d->path, sizeof d->path, "%s", spath);
            hid_set_nonblocking(h, 1);   /* macOS 不给 fd，多设备只能非阻塞轮询 */

            printf("✅ 已连接 [%s pid=0x%04X] 鼠标报告 ID=0x%02X 按键字节=buf[%d]"
                   " 声明 %d 个按键位\n"
                   "   （共享方式打开，不抢占系统鼠标驱动）\n",
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
    printf("⚠️  设备 [%s pid=0x%04X] 已断开（拔出/切换连接方式/休眠）\n",
           pid_name(g_devs[idx].pid), g_devs[idx].pid);
    hid_close(g_devs[idx].h);
    g_devs[idx] = g_devs[g_ndevs - 1];   /* 用末项补位，保持数组紧凑 */
    g_ndevs--;
}

static void close_all_devs(void)
{
    while (g_ndevs > 0) {
        hid_close(g_devs[g_ndevs - 1].h);
        g_ndevs--;
    }
}

/* 一份原始报告：只认解析出来的鼠标报告，其余（多媒体/键盘/厂商配置）忽略 */
static void handle_report(jz_dev *d, const unsigned char *buf, int len)
{
    int     is_mouse = (len > d->btn_off && buf[0] == d->report_id);
    uint8_t btn      = is_mouse ? buf[d->btn_off] : 0;

    /* --probe 只打印“有意思”的：非鼠标报告 + 按键位变化，
     * 否则每 8ms 一次的移动报告会刷屏 */
    if (g_probe && (!is_mouse || btn != d->last_btn)) {
        printf("  [%s] report[%2d]:", pid_name(d->pid), len);
        for (int i = 0; i < len && i < 8; i++)
            printf(" %02X", buf[i]);
        printf("%s\n", is_mouse ? "   <- 鼠标报告，按键位图" : "");
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

/*
 * 把“TCC 此刻是按哪个身份在给我们记账”打出来。
 * 排查「勾了开关却仍是已拒绝」全靠它：授权记录是绑在“责任进程”上的，
 * LaunchAgent 直接跑 bundle 里的可执行文件时，责任进程可能并不是那个
 * .app（列表里就会出现一条和实际身份无关的记录，怎么勾都没用）。
 */
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

/*
 * 主动向 TCC 申请「输入监控」。
 * 这一步很关键：后台进程（LaunchAgent）自己去 open 设备时只会默默拿到
 * 0xE00002E2，系统不弹窗、「输入监控」列表里也不会出现这一项，用户根本没有
 * 地方可勾。IOHIDRequestAccess 才会走正规的申请流程 —— 弹窗 + 在列表里登记。
 * 注意：之前被拒绝过（Denied）时系统不会再弹窗，只能手动勾选。
 */
static void request_input_monitoring(void)
{
    if (dump_auth_state("startup") == kIOHIDAccessTypeGranted) {
        printf("「输入监控」已授权\n");
        return;
    }

    /* 实测：一个从没在这台机器上登记过的 App，IOHIDCheckAccess 也返回
     * Denied —— macOS 把“没有记录”和“用户明确拒绝”混成了同一个值（tccutil
     * reset 之后立刻再查还是 Denied，可证）。所以不能拿这个状态决定“要不要
     * 申请”：只有 IOHIDRequestAccess 这一次调用才会弹窗、才会把本 App 登记
     * 进「输入监控」列表。漏掉它就表现为“默默被拒，列表里啥也没有”。 */
    printf("正在请求「输入监控」授权……（弹窗可能被其它窗口挡住）\n");

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
           "（授权对象是“责任进程”：终端里跑就是终端，LaunchAgent 跑就是那个 .app）\n"
           "本程序以共享方式(kIOHIDOptionsTypeNone)打开设备，不会抢占系统鼠标驱动；\n"
           "同一时间只允许一个实例，避免一次侧键触发两遍快捷键。\n",
           argv0);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    setvbuf(stdout, NULL, _IOLBF, 0);   /* 重定向到日志文件时也能逐行实时输出 */
    const char *argv0 = argv[0];

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe"))          g_probe = 1;
        else if (!strcmp(argv[i], "--check"))     g_check = 1;
        else if (!strcmp(argv[i], "--auth-probe")) {
            /* 只问一句“TCC 认的是哪个身份、状态如何”就退出，不动设备。
             * 结果同时落盘：用 `open --args --auth-probe` 启动时 stdout 会被丢掉，
             * 只有文件看得到，而那种启动方式的身份才是 .app 自己。 */
            if (freopen("/tmp/jzm3_auth_probe.log", "a", stdout))
                fprintf(stderr, "（结果也写进了 /tmp/jzm3_auth_probe.log）\n");
            dump_auth_state("auth-probe");
            return 0;
        }
        else if (!strcmp(argv[i], "--auth-request")) {
            /* 主动走一次系统授权申请（会弹窗，阻塞到用户回答）。
             * 配合 `open <.app> --args --auth-request` 用，身份就是 .app 自己。 */
            if (freopen("/tmp/jzm3_auth_probe.log", "a", stdout))
                fprintf(stderr, "（结果写进了 /tmp/jzm3_auth_probe.log）\n");
            request_input_monitoring();
            return 0;
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv0);
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n\n", argv[i]);
            usage(argv0);
            return 2;
        }
    }

    /* 单实例锁：共享打开允许多个客户端同时读同一设备，所以如果终端里
     * 跑了一份、LaunchAgent 又起了一份，一次侧键会触发两遍快捷键 */
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

    /* ★ 修复的核心：默认值是 1（独占/Seize），必须改成 0（共享） */
    hid_darwin_set_open_exclusive(0);
    if (hid_darwin_get_open_exclusive() != 0) {
        fprintf(stderr, "❌ 无法关闭独占打开（hid_darwin_get_open_exclusive 仍返回非 0）\n"
                        "   请确认链接的是 hidapi 0.14.0 及以上版本：brew install hidapi\n");
        hid_exit();
        return 1;
    }

    /* 后台跑时系统不会代为弹窗，必须自己申请一次；否则「输入监控」列表里
     * 连这一项都不会出现，用户根本无从勾选 */
    request_input_monitoring();

    /* 打印构建时间戳和打开方式，避免误跑旧版二进制 */
    printf("build: %s %s   打开方式: %s\n", __DATE__, __TIME__,
           hid_darwin_get_open_exclusive() == 0
               ? "共享 kIOHIDOptionsTypeNone（不抢占鼠标）"
               : "独占 Seize（会冻住鼠标！）");
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

        /* 双保险：万一又被独占打开，立刻关掉，否则鼠标会被冻住 */
        for (int i = 0; i < g_ndevs; i++) {
            if (hid_darwin_is_device_open_exclusive(g_devs[i].h)) {
                fprintf(stderr, "⚠️  [%s] 仍以独占方式打开，正在关闭\n",
                        pid_name(g_devs[i].pid));
                hid_darwin_set_open_exclusive(0);
                close_dev(i);
                i = -1;              /* 末项补过位，从头再查一遍 */
            }
        }
        if (g_ndevs == 0)
            continue;

        if (g_check) {
            printf("✅ 自检通过。现在移动/点击/滚轮都应正常，侧键触发 Cmd+[ 与 Cmd+]\n");
            close_all_devs();
            free(buf);
            hid_exit();
            return 0;
        }

        /* 轮询所有在线设备：macOS 上 hidapi 不暴露 fd，没法 select，
         * 只能非阻塞读 + 4ms 一拍（比鼠标 8ms 的报告周期细，不漏按键） */
        while (g_ndevs > 0) {
            int got = 0;

            for (int i = 0; i < g_ndevs; ) {
                int n = hid_read(g_devs[i].h, buf, 64);
                if (n < 0) {
                    close_dev(i);    /* 拔出 / 休眠 / 换到别的连接方式 */
                    last_scan = 0;   /* 少了一个，马上补扫，别等下一拍 */
                    continue;        /* 末项补到了 i 上，不递增 */
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

            /*
             * 关键：只要 2.4G 接收器还插着，它的句柄就一直“在线但静默”，
             * g_ndevs 永远不为 0，光靠“全断开才重扫”就永远发现不了刚连上的
             * 蓝牙。所以定期补扫一次（open_all_jzm3 只收新 path，不动已开的）。
             */
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

            /* --probe：每约 2 秒汇报一次收没收到报告，用来区分
             * “读不到数据”和“读到了但不是侧键” */
            if (g_probe && ++idle_rounds >= 500) {
                idle_rounds = 0;
                if (total_reports != ticked) {
                    printf("  （2 秒内共收到 %ld 份报告）\n", total_reports - ticked);
                    ticked = total_reports;
                    silent_hinted = 0;
                } else if (!silent_hinted) {
                    silent_hinted = 1;
                    printf("  （静默中——鼠标不动时本来就没有报告；"
                           "请移动一下鼠标确认链路，再按侧键）\n");
                }
            }
        }

        sleep(2);   /* 全部断开：回到外层重新枚举（切回 2.4G/蓝牙都能自动接上） */
    }

    free(buf);
    hid_exit();
    return 0;
}
