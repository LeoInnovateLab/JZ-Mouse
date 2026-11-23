#!/bin/bash
# 把 JZ M3 侧键守护进程装成后台服务：开机/登录自启，全局生效，不依赖终端。
#
#   ./install.sh            安装（会自动编译）
#   ./install.sh --remove   卸载
#
# 为什么要 .app + LaunchAgent：
#   TCC「输入监控」记的是“责任进程”。终端里手动跑，授权对象就是终端；
#   LaunchAgent 跑裸二进制，责任进程会算到 launchd 上，同样没法授权。
#   包成 .app 之后，列表里出现的是它自己，授权才稳定、才与终端无关。
#
# 关键前提：整台机器上只能有一份这个 bundle id 的 .app。TCC 是按
# bundle id 去查授权记录的，多份同名 .app 时它可能解析到没被勾选的那份，
# 症状就是“明明勾了，还是 0xE00002E2 not permitted”。
set -euo pipefail
cd "$(dirname "$0")"

LABEL=com.jzm3.side-buttons
APP_NAME=JZMSideButtons.app
BIN_NAME=jzm3_side_buttons
UID_NUM=$(id -u)
GUI="gui/$UID_NUM"
APP_DIR="$HOME/Applications"
DST_APP="$APP_DIR/$APP_NAME"
STAGE_APP="build/$APP_NAME"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/jzm3_side_buttons.log"
LSREGISTER="/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister"

if [ "${1:-}" = "--remove" ]; then
    launchctl bootout "$GUI/$LABEL" 2>/dev/null || true
    rm -f "$PLIST"
    rm -rf "$DST_APP"
    "$LSREGISTER" -u "$DST_APP" 2>/dev/null || true
    echo "🧹 已卸载：服务已停止，$PLIST 与 $DST_APP 已删除。"
    echo "   日志 $LOG 保留着；「输入监控」里那条记录顺手删掉即可。"
    exit 0
fi

# 1) 编译，产出 build/JZMSideButtons.app
./build.sh > /dev/null
[ -d "$STAGE_APP" ] || { echo "❌ 没找到 $STAGE_APP，build.sh 可能失败了"; exit 1; }

# 2) 只往 ~/Applications 装一份，并把机器上其它同名副本清掉
mkdir -p "$APP_DIR"
rm -rf "$DST_APP"
cp -R "$STAGE_APP" "$DST_APP"
rm -rf "$STAGE_APP" "$APP_NAME"     # 暂存副本 + 旧版留在仓库根目录的副本
codesign --force --sign - --identifier "$LABEL" "$DST_APP" 2>/dev/null || true
"$LSREGISTER" -f "$DST_APP" 2>/dev/null || true

OTHERS=$(mdfind "kMDItemCFBundleIdentifier == '$LABEL'" 2>/dev/null | grep -vx "$DST_APP" || true)
[ -n "$OTHERS" ] && echo "⚠️  还发现别的同 bundle id 副本，建议删掉以免 TCC 认错：$OTHERS"

# 3) 写 LaunchAgent
mkdir -p "$HOME/Library/LaunchAgents" "$(dirname "$LOG")"
cat > "$PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>$LABEL</string>
    <key>ProgramArguments</key>
    <array>
        <string>$DST_APP/Contents/MacOS/$BIN_NAME</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>ThrottleInterval</key>
    <integer>10</integer>
    <key>ProcessType</key>
    <string>Interactive</string>
    <key>StandardOutPath</key>
    <string>$LOG</string>
    <key>StandardErrorPath</key>
    <string>$LOG</string>
</dict>
</plist>
PLIST
plutil -lint "$PLIST" > /dev/null

# 4) 加载并立刻跑一次（bootout 只为换掉旧定义；不去 pkill 别的实例，
#    两份同时跑会让一次侧键触发两遍快捷键，那事交给二进制里的 flock 锁）
launchctl bootout "$GUI/$LABEL" 2>/dev/null || true
launchctl bootstrap "$GUI" "$PLIST"
: > "$LOG"
launchctl kickstart -k "$GUI/$LABEL"

# 5) 等第一轮结果，直接告诉用户成没成
ok=0
denied=0
for _ in 1 2 3 4 5 6 7 8; do
    sleep 2
    if grep -q "已连接（共享模式）" "$LOG" 2>/dev/null; then ok=1; break; fi
    if grep -q "not permitted"      "$LOG" 2>/dev/null; then denied=1; break; fi
done

echo "📄 日志（$LOG）："
echo "----------------------------------------------------------------"
tail -n 12 "$LOG" 2>/dev/null || echo "(还没有日志)"
echo "----------------------------------------------------------------"
echo

if [ "$ok" = 1 ]; then
    echo "✅ 服务已安装并在运行：$DST_APP"
    echo "   侧键 -> Cmd+[ / Cmd+]，在所有前台 App 里都生效（与 iTerm 无关）。"
elif [ "$denied" = 1 ]; then
    echo "⛔ 服务在跑，但还没拿到「输入监控」授权。本程序启动时已经自己申请过"
    echo "   一次，所以列表里现在应该能看到它（系统弹窗也可能被别的窗口挡住）："
    echo "   1) 系统设置 > 隐私与安全性 > 输入监控"
    echo "      打开「JZ M3 Side Buttons / jzm3_side_buttons」的开关；列表里找不到"
    echo "      就用 + 手动添加：$DST_APP"
    echo "      （若有失效的旧记录挡着，可先清掉："
    echo "        tccutil reset ListenEvent $LABEL）"
    echo "   2) 重启服务：  launchctl kickstart -k $GUI/$LABEL"
    echo "      （也不用急：守护进程每 10 秒重试一次，勾上后约 10 秒内自己就通了）"
    echo "   3) 看结果：    tail -f $LOG      （出现「✅ 已连接（共享模式）」即成功）"
    open "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent" || true
else
    echo "⚠️  状态未知，手动看日志： tail -f $LOG"
fi

echo
echo "常用命令："
echo "  看日志        tail -f $LOG"
echo "  重启服务      launchctl kickstart -k $GUI/$LABEL"
echo "  停止服务      launchctl bootout $GUI/$LABEL"
echo "  卸载          ./install.sh --remove"
echo
echo "注意：终端里手动跑的那份和后台这份只会生效一份（flock 单实例锁）。"
echo "      想临时用 --probe 调试，先停掉后台的：launchctl bootout $GUI/$LABEL"
echo "      重新跑 install.sh 会重新签名，之后需要在「输入监控」里再勾一次。"
