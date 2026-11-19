# FitBuds Tray Win

EDIFIER FitBuds Pro 的 Windows 托盘控制器。右键托盘图标可查看耳机与充电仓电量，切换降噪、环境声、音效和游戏模式，也可以关闭耳机。单耳使用时，菜单会禁用不可用的降噪模式。

## 使用

在 Windows 蓝牙设置中配对 EDIFIER FitBuds Pro，双击 `FitBudsTray.exe`。程序会自动查找名称包含 `FitBuds` 的已配对设备，有多个匹配时优先选择已连接的设备。配对后可右键托盘图标，选择“刷新电量与设置”。

## 从源码构建

将 MinGW-w64 的 `g++` 和 `windres` 加入 PATH，在 PowerShell 中运行 `./build.ps1`。也可以运行 `./fitbuds.exe battery` 在终端查询电量。
