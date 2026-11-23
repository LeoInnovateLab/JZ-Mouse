# JZ M3 Side Buttons

京造 JZ M3 鼠标侧键映射工具（macOS），将侧键映射为 `Cmd+[` / `Cmd+]`（浏览器前进/后退）。

## 特性

- 支持 2.4G 接收器 (PID 0x8373) 和蓝牙 (PID 0x8266) 两种连接模式
- 共享方式打开设备，不抢占系统鼠标驱动（光标、点击、滚轮正常）
- 从 HID Report Descriptor 动态识别报告格式，不硬编码
- 自动过滤复合设备的键盘接口，避免误触发
- 连接模式切换时自动重枚举，无需重启进程
- 单实例锁，防止重复触发

## 系统要求

- macOS 11.0+
- [hidapi](https://github.com/libusb/hidapi)：`brew install hidapi pkg-config`
- Xcode Command Line Tools（编译用）

## 编译

```bash
./build.sh
```

产物：`jzm3_side_buttons`（二进制）+ `build/JZMSideButtons.app`

## 安装为后台服务

```bash
./install.sh            # 安装（自动编译 + 注册 LaunchAgent）
./install.sh --remove   # 卸载
```

安装后：
- 开机/登录自动启动
- 全局生效，不依赖终端
- 日志：`~/Library/Logs/jzm3_side_buttons.log`

## 权限配置

该设备需要 macOS「输入监控」+「辅助功能」两项权限。

### 输入监控（Input Monitoring）

安装后首次运行会提示授权。若列表里没有，手动添加：

1. 系统设置 → 隐私与安全性 → **输入监控**
2. 点 `+`，`⌘⇧G` 输入 `/Users/leo/Applications/JZMSideButtons.app`
3. 选中 .app 本身，打开开关

### 辅助功能（Accessibility）

合成键盘事件需要此项权限，否则侧键信号会被系统丢弃：

1. 系统设置 → 隐私与安全性 → **辅助功能**
2. 同样方式添加 `JZMSideButtons.app` 并打开开关

### 授权失效处理

每次 `install.sh` 会重新签名，导致授权记录失效。若出现"勾了仍无效"：

```bash
tccutil reset ListenEvent com.jzm3.side-buttons
tccutil reset Accessibility com.jzm3.side-buttons
launchctl kickstart -k gui/501/com.jzm3.side-buttons
```

然后重新在两项权限里添加 .app 并打开开关。

## 手动运行（调试用）

```bash
./jzm3_side_buttons --check     # 自检后退出
./jzm3_side_buttons --probe     # 打印原始 HID 报告
./jzm3_side_buttons --auth-probe  # 查看 TCC 授权状态
```

注意：手动运行需先停掉后台服务（`launchctl bootout gui/501/com.jzm3.side-buttons`），否则单实例锁会阻止启动。

## 故障排查

| 现象 | 可能原因 | 解决 |
|------|---------|------|
| `not permitted` | 缺少输入监控授权 | 按上述步骤授权 |
| 侧键日志有但浏览器无反应 | 缺少辅助功能授权 | 添加辅助功能授权 |
| 2.4G 正常但蓝牙无效 | 蓝牙未连接或句柄未打开 | 检查系统蓝牙设置；日志应有 `✅ 已连接 [蓝牙 ...]` |
| 一次按键触发两次 | 多实例运行 | `launchctl bootout gui/501/com.jzm3.side-buttons` 停掉后台 |
| 鼠标不能动/不能点 | 旧版独占打开 bug | 确认日志显示"共享方式打开" |

## 技术细节

- **根因修复**：hidapi macOS 默认以 `kIOHIDOptionsTypeSeizeDevice` 独占打开设备，导致系统鼠标驱动收不到事件。调用 `hid_darwin_set_open_exclusive(0)` 改为共享模式。
- **为什么不用 CGEventTap**：该鼠标的 `HIDPointerButtonCount = 3`，侧键两位被当作 padding，永不产生 `otherMouseDown` 事件，只能读原始 HID 报告。
- **报告格式**：2.4G 鼠标报告 ID 0x01，按键位图在 `buf[1]`（bit3=后侧键 0x08，bit4=前侧键 0x10）；蓝牙报告 ID 0x03，布局相同。

## License

See [LICENSE](LICENSE)
