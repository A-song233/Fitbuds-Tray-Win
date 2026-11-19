# FitBuds Tray Win

EDIFIER FitBuds Pro 的 Windows 托盘控制器。右键托盘图标可查看耳机与充电仓电量，切换降噪、环境声、音效和游戏模式，也可以关闭耳机。单耳使用时，菜单会禁用不可用的降噪模式。

## 构建与使用

1. 在 Windows 上配对并连接耳机，然后在 PowerShell 中运行 `Get-PnpDevice -Class Bluetooth | Where-Object InstanceId -like 'BTHENUM\DEV_*' | Format-Table FriendlyName, InstanceId`。找到 FitBuds Pro 对应的 `DEV_` 后 12 位十六进制地址。
2. 将 `FitBudsTray.example.ini` 复制为 `FitBudsTray.ini`，把地址填到 `address=` 后面。
3. 将 MinGW-w64 的 `g++` 和 `windres` 加入 PATH，运行 `./build.ps1`，然后启动 `FitBudsTray.exe`。也可以运行 `./fitbuds.exe battery` 在终端查询电量。
