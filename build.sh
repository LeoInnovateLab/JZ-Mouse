#!/bin/bash
# 编译 JZ M3 侧键映射守护进程（需要 hidapi：brew install hidapi）
set -euo pipefail
cd "$(dirname "$0")"

OUT=jzm3_side_buttons
PKGCFG=$(pkg-config --cflags --libs hidapi 2>/dev/null || echo "-L/opt/homebrew/lib -lhidapi")
# 源码用的是 <hidapi/hidapi.h> 这种带目录的写法，补上 .../include 这一层
HIDINC=""
for p in "$(brew --prefix hidapi 2>/dev/null || true)" /opt/homebrew /usr/local; do
    [ -n "$p" ] && [ -d "$p/include/hidapi" ] && HIDINC="-I$p/include" && break
done
[ -z "$HIDINC" ] && echo "⚠️  没找到 hidapi 头文件，请先: brew install hidapi pkg-config"

cc -O2 -Wall -Wextra $HIDINC $PKGCFG \
   -framework ApplicationServices -framework CoreFoundation -framework IOKit \
   -o "$OUT" jzm3_side_buttons.c

# 固定签名标识：重新编译后 TCC 授权不容易失效
codesign --force --sign - --identifier com.jzm3.side-buttons "$OUT" 2>/dev/null || true

# ---------------------------------------------------------------------
# 顺手打一个 .app：LaunchAgent 跑裸二进制时，TCC「输入监控」的授权对象
# 会算到 launchd/终端头上；跑 .app 才认它自己（有 CFBundleIdentifier）。
# 这就是「不只在 iTerm 里生效、开机常驻」的关键一步。
#
# 只允许磁盘上存在一份这个 bundle id 的 .app：TCC 是按 bundle id 找授权
# 记录的，两份同名 .app 会让它解析到没被勾选的那份，表现为“勾了仍 not
# permitted”。所以产物放 build/，仓库根目录下的旧副本一并清掉。
# ---------------------------------------------------------------------
APP=JZMSideButtons.app
rm -rf "$APP"                    # 清掉旧版留在仓库根目录的副本
mkdir -p build
APP_DIR_OUT=build
rm -rf "$APP_DIR_OUT/$APP"
mkdir -p "$APP_DIR_OUT/$APP/Contents/MacOS"
cp "$OUT" "$APP_DIR_OUT/$APP/Contents/MacOS/$OUT"
cat > "$APP_DIR_OUT/$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>            <string>jzm3_side_buttons</string>
    <key>CFBundleIdentifier</key>            <string>com.jzm3.side-buttons</string>
    <key>CFBundleName</key>                  <string>JZ M3 Side Buttons</string>
    <key>CFBundlePackageType</key>           <string>APPL</string>
    <key>CFBundleShortVersionString</key>    <string>1.0</string>
    <key>CFBundleVersion</key>               <string>1</string>
    <key>LSMinimumSystemVersion</key>        <string>11.0</string>
    <key>LSUIElement</key>                   <true/>
</dict>
</plist>
PLIST
# 整包签名（会连里面的可执行文件一起签）
codesign --force --sign - --identifier com.jzm3.side-buttons "$APP_DIR_OUT/$APP" 2>/dev/null || true

echo "编译完成：$(pwd)/$OUT"
echo "应用包：  $(pwd)/$APP_DIR_OUT/$APP"
echo
echo "运行顺序："
echo "  1) ./$OUT --check     # 只验证能否以共享方式打开设备（排查权限）"
echo "  2) ./$OUT --probe     # 打印原始报告，核对侧键位图"
echo "  3) ./install.sh       # 装成后台服务：开机自启、全局生效、不依赖终端"
echo
echo "权限：系统设置 > 隐私与安全性 > 输入监控"
echo "  • 终端里手动跑 -> 勾选 iTerm/Terminal，完全退出终端再重开"
echo "  • install.sh 装成服务 -> 勾选 ~/Applications/JZMSideButtons.app"
echo "  不需要「辅助功能」权限。"
