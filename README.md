# TrafficBar

一个 Windows 10 任务栏状态栏小工具，用 C++ / Win32 编写。

完整使用说明：[docs/使用说明.md](docs/使用说明.md)

显示：

- 实时上传 / 下载速度
- CPU 使用率 / 温度（ACPI 可用时）
- 内存使用率
- GPU 使用率 / 温度（可选，NVIDIA 温度）
- 按日 / 按月 / 按年历史流量统计
- 磁盘 SMART 健康 / 温度（可用时）
- 插件自定义指标（`plugins` 目录）

界面为两行紧凑布局，贴在任务栏通知区域左侧，跟随任务栏高度、DPI 和深浅色主题。任务栏在左侧/右侧时自动切换为窄条纵向布局；宽度小于 110px 时各项目和百分比拆成两行显示。多显示器副任务栏会自动发现并显示，同样支持悬停浮帖和右键菜单。

## 构建

环境：Visual Studio 2022（MSVC v143）+ Windows SDK 10.0.19041。

```bat
build.bat
```

输出：`build\TrafficBar.exe`（x64，静态链接 CRT）。交付副本在 `dist\TrafficBar.exe`。

如果 VS 安装路径不同，修改 `build.bat` 里的 `vcvars64.bat` 路径即可。

## 运行

直接运行 `TrafficBar.exe`。

程序带 `requireAdministrator` 清单，启动时会请求管理员权限；这是任务栏置顶、托盘和后续温度监控所需。若系统弹出 UAC 提示，请允许运行。

首次运行后，右键任务栏文字或托盘图标可以打开菜单：

- 显示 CPU / 显示内存 / 显示 GPU（悬停）
- 外观主题：跟随任务栏 / 深色 / 浅色 / 深蓝 / 墨绿
- 网卡：自动选择或手动指定
- 刷新间隔：0.5 / 1 / 2 / 5 秒
- 开机自启（任务计划，最高权限运行）
- 流量统计...：打开流量统计窗口
- 进程流量...：打开按进程统计窗口
- 关于 / 退出

隐藏快捷键：

- `Ctrl+Alt+S`：打开“自定义指标”设置窗

双击任务栏文字打开任务管理器。

也可以带参数启动：

```bat
TrafficBar.exe --history
TrafficBar.exe --process-traffic
TrafficBar.exe --safe-mode
```

`--safe-mode` 会跳过插件、ETW 进程流量、GPU/WMI 温度和 SMART 查询，只保留核心网速、CPU、内存、磁盘和行情功能，用于排查系统级异常。

如果已有实例在运行，这些命令会直接打开对应的窗口。

## 配置与数据

程序目录下自动生成 `TrafficBar.ini`：

```ini
[general]
show_cpu=1
show_memory=1
show_disk=0
show_gpu=0
theme_mode=0
interval_ms=1000
font_size=9
adapter_luid=0000000000000000
```

`adapter_luid=0` 表示自动选择。删除该文件可恢复默认值。

历史流量保存在同目录的 `TrafficBar_history.dat`，文本格式：

```text
TrafficBarHistory 1
2026-11-12 7498265 33545046
```

每行是 `日期 上传字节数 下载字节数`，按本机日期归档。程序每 60 秒、打开统计窗口、退出和系统关机时自动保存；写入时先写临时文件，并保留 `.bak` 备份。

> 统计范围是 TrafficBar 运行期间；程序关闭期间的流量不计。首次启用时从当天开始记录。

## 历史流量窗口

- 支持按日 / 按月 / 按年查看，列表按选择的时间粒度倒序汇总
- 顶部汇总：今日、本月、今年、总计的上传 / 下载 / 合计
- 深色/浅色主题自动适配
- 支持清空历史（会二次确认）
- 数据仅保存在本机，不联网

## 进程流量（按进程）

- 右键菜单选择“进程流量...”
- 使用 Windows ETW 的 `Microsoft-Windows-Kernel-Network` 统计 TCP / UDP 发送和接收字节；
- 显示进程名、PID、实时上传/下载速率和累计流量；
- 统计范围为本程序运行期间，需要管理员权限。

## 插件

将 64 位 DLL 放到 `TrafficBar.exe` 同目录的 `plugins` 文件夹，导出：

```cpp
struct TrafficBarPluginMetric {
    wchar_t label[32];
    wchar_t value[64];
};

extern "C" __declspec(dllexport) int __cdecl
TrafficBarPluginGetMetrics(TrafficBarPluginMetric* metrics, int capacity);
```

返回写入的指标数量，最多显示 4 项；插件指标会追加到悬停浮帖。接口头文件在 `plugins/api/TrafficBarPlugin.h`。

## 自定义指标（可选）

任务栏第二行可以显示一个主指标，鼠标悬停显示全部项目。默认关闭，使用 `Ctrl+Alt+S` 打开设置；设置窗不会出现在右键菜单和托盘提示里。

每行填写一个 `名称=标识`：

- 标识支持 `sh` / `sz` / `bj` / `hk` / `us` 前缀；
- 也可以直接填 6 位 A 股代码、5 位港股代码或美股 ticker；
- 名称建议用 2–4 个字母的缩写，任务栏只显示名称、价格和涨跌，不显示“股票”等字样；
- 默认不显示百分号，可在配置里设置 `show_percent=1`。

```ini
[custom]
enabled=1
primary=AAPL
items=AAPL=usAAPL;MSFT=usMSFT;TX=hk00700
refresh_ms=5000
show_percent=0
```

行情来自腾讯行情接口，A/HK/US 自动识别；交易时段每 5 秒刷新，非交易时段降低频率。网络失败时保留上一次有效值，不弹错误框。

## 鼠标悬停浮帖

鼠标移到任务栏文字上约 300 ms，会显示一个小浮帖：

- 当前上传 / 下载速度
- CPU 使用率
- 实际已用内存 / 物理内存总量 / 百分比
- 可选显示系统盘占用与 SMART：磁盘 C: 38% 正常 26°C
- CPU 温度可用时与占用一起显示：CPU 8%  45°C
- 可选显示 GPU 使用率：GPU 12%
- 如果检测到 NVIDIA 驱动，同时显示 GPU 温度：GPU 12%  42°C
- 自选指标超过 3 个时，列表固定显示 3 行并出现内部滚动条，可用鼠标滚轮或拖动滚动条查看
- 浮帖背景跟随任务栏主题色变化

鼠标离开后自动隐藏；打开右键菜单、历史窗口或通知区面板时不会覆盖这些界面。

## 实现说明

- 任务栏定位：查找 `Shell_TrayWnd` 和 `TrayNotifyWnd`，把无边框置顶窗口放在通知区域左侧；不注入 Explorer、不装驱动。
- 采样：`GetIfTable2`（64 位网卡计数器）、`GetSystemTimes`、`GlobalMemoryStatusEx`。
- 历史：采样器输出每个刷新周期的字节增量，`HistoryStore` 按本地日期累加；网卡切换/休眠恢复时重置基线，不会把旧计数差额算进历史。
- 绘制：GDI 双缓冲；背景色从任务栏采样，文字颜色按亮度自动切换深浅色。
- 置顶守护：每 150 ms 检查窗口是否被任务栏/桌面遮挡；如果是则重新置顶。开始菜单、通知区溢出面板和右键菜单打开时不会强行覆盖。桌面被点成前台窗口时不会误判为全屏应用而隐藏监控。
- 单实例：命名 Mutex + 枚举窗口；第二次启动会唤起已有实例的菜单或历史窗口。
- 开机自启：旧版 HKCU Run 项会自动迁移到 Task Scheduler 登录任务，避免 UAC 阻止自启。
- 诊断日志：启动/退出写入 `TrafficBar.log`，便于排查异常重启或系统卡死时程序是否在运行。
- Explorer 重启：监听 `TaskbarCreated`，重新注册托盘图标并重新定位。

## 已实现 / 未实现

已实现：网速、CPU、CPU 温度（ACPI）、内存、GPU 使用率/温度（NVIDIA）、磁盘占用、磁盘 SMART 健康/温度、插件指标、悬停浮帖、按日/月/年历史流量统计、按进程流量统计、自定义指标（A/HK/US 行情）、任务栏左/右竖排布局、多显示器副任务栏、托盘、右键菜单、网卡选择、刷新间隔、外观/皮肤预设、配置持久化、单实例、任务计划开机自启、DPI/主题适配。

暂未实现：

- Windows 11 专门适配（当前只保证 Windows 10）
- 自定义皮肤、非 NVIDIA GPU 温度、GPU 显存

## 目录

```text
TrafficBar/
  src/             C++ 源码
  resources/       图标与 manifest
  docs/            截图
  app.rc           资源脚本
  build.bat        构建脚本
  build/           构建输出
  dist/            发布副本
```

## License

MIT License. See [LICENSE](LICENSE).
