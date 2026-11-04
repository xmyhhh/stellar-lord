# 星际领主（Stellar Lord）

> **新版设计文档**
>
> 本仓库已更名为「星际领主 / Stellar Lord」，联机入口为「加入时空」。
> 新的世界观、剧本、玩法与联机设计见 [`docs/README.md`](docs/README.md)。
> 本 README 其余内容描述当前原型，代码迁移完成后再更新。

一个常驻 Windows 任务栏的挂机放置游戏，**外观是系统性能监视器**。

任务栏控件默认显示真实的 CPU / 内存 / 显卡 / 网络 / 磁盘数据，点开也只是进程列表；
按 `Alt+Q` 才会切成「领主面板」——采矿、熔炼、派船、跑星际航线、处理待办。

设计目标：上班可玩、随时中断、单次操作 30 秒内、无音效无弹窗无红点。

---

## 构建

依赖：

- Windows 10 / 11
- CMake 3.20+
- Visual Studio 2022（MSVC，x64）
- PowerShell（用于从 `logo.png` 生成图标）

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build/Release/stellar_lord_taskbar.exe`

跑测试（纯逻辑层，不依赖 Win32）：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## 运行

直接双击 `stellar_lord_taskbar.exe`。程序没有主窗口，只会嵌入任务栏并驻留托盘。

> 首次运行会在 exe 同目录生成 `config.json` 和 `stellar_lord_save.txt`。
> 因此不要放在 `C:\Program Files` 之类无写权限的目录，建议单独放一个文件夹。

---

## 交互

| 操作 | 效果 |
| --- | --- |
| `Alt+Q`（可改） | 在「性能监视」和「星际领主」之间切换 |
| 悬停 / 左键点击任务栏控件 | 打开弹窗（方式可在菜单里改） |
| 右键任务栏控件或托盘图标 | 菜单：模式、弹窗方式、刷新间隔、快捷键、语言、开机启动、清空存档、退出 |
| 左键单击托盘图标 | 快速切换模式 |

弹窗内键盘操作：

| 按键 | 效果 |
| --- | --- |
| `1` `2` `3` `4` | 切换「生产 / 航线 / 待办 / 研究」 |
| `←` `→` | 切换标签 |
| `↑` `↓` | 选择行 |
| `Enter` / `Space` | 执行选中行 |
| `Q` / `E` | 待办事件选左 / 右选项 |
| `Esc` | 关闭弹窗 |
| 滚轮 | 切换标签 |

弹窗里的行都是可点的：点一下就是执行（升级设施、买船、派遣、买研究、上市）。

---

## 玩法

**生产链**：采矿机 → 熔炉 → 零件厂 → 装配线

```
矿石 ORE --熔炉--> 合金 ALY --零件厂--> 零件 PRT --装配线--> 货舱 CRG
```

- 每条产线只吃上游库存，不会凭空产出；上游不够就是空转。
- 升级消耗**对应上游资源**：采矿机/熔炉吃 ORE，零件厂吃 ALY，装配线吃 PRT。

**航线**：货舱装船 → 派出 → 到港结算星币。

| 航线 | 时长 | 单价 | 风险 |
| --- | --- | --- | --- |
| K-7 内环 | 15 分钟 | 10 / 货舱 | 0% |
| V-2 矿区 | 2 小时 | 40 / 货舱 | 8% |
| X-9 深空 | 8 小时 | 170 / 货舱 | 20% |

- 船队上限 8 艘，每艘每趟装 `4 + 单船容量研究等级` 个货舱。
- 风险触发会生成**待办事件**（海盗 / 离子风暴 / 顺风急单），等你回来处理。

**待办**：每条事件有两个选项（稳妥 / 赌一把），也可以「一键处理全部」按稳妥选项结算。
待办最多 8 条，超出时最旧的一条自动按稳妥选项结算，不会卡死。

**研究**：用「上市」换来的研究点升级，最高 5 级。

- 离线效率：离线产出倍率 55% → 95%
- 全局产能：每级 +15%
- 单船容量：每级 +1 货舱
- 事件自动处理：自动结算航次事件（高风险按 85% 折价）

**上市（转生）**：累计赚到 25 万星币后可以「公司上市」，重置设施 / 船队 / 星币，
换取研究点（`sqrt(累计收益 / 1000)` 向下取整）。研究点与研究等级保留。

**离线结算**：退出后按 `Date.now` 差值补算，最多累计 **72 小时**（覆盖周末）。
离线效率只影响产出，不影响航次到达时间——下班派的长途船，第二天回来就是已到港状态。

---

## 配置

`config.json`（与 exe 同目录）：

```json
{
  "show_cpu": true,
  "show_memory": true,
  "show_upload": true,
  "show_download": true,
  "show_gpu": true,
  "show_disk_read": true,
  "show_disk_write": true,
  "network_unit": "bits",
  "monitor_popup_mode": "hover",
  "lord_popup_mode": "click",
  "toggle_hotkey": "alt_q",
  "language": "zh_cn",
  "sample_interval_seconds": 1,
  "taskbar_monitor_index": 0,
  "auto_start": false
}
```

| 字段 | 说明 |
| --- | --- |
| `show_*` | 监视器模式下任务栏显示哪些指标 |
| `network_unit` | `bits` / `bytes` |
| `monitor_popup_mode` | 监视器模式弹窗方式：`hover` / `click` |
| `lord_popup_mode` | 领主模式弹窗方式：`hover` / `click` |
| `toggle_hotkey` | `alt_q` / `alt_s` / `alt_m` / `ctrl_alt_q` |
| `language` | `zh_cn` / `en` |
| `sample_interval_seconds` | `1` / `2` / `5` / `10` |
| `taskbar_monitor_index` | 多显示器时目标任务栏序号（0 = 主屏） |
| `auto_start` | 开机启动（写入 `HKCU\...\Run`） |

`stellar_lord_save.txt` 是纯文本存档，一行一个字段，可以直接手改（改之前先退出程序）。

---

## 目录结构

```
src/
  main.cpp             Win32 UI、任务栏控件绘制、弹窗与交互、游戏 tick/存档
  game_model.h/.cpp    纯逻辑层：生产链、航线、事件、研究、上市、存档序列化
  app_config.h/.cpp    配置读写、存档路径
  taskbar_embedder.*   任务栏嵌入（Win10 / Win11）
  system_metrics.*     系统指标采样与格式化
  process_monitor.*    进程列表采样（监视器模式的伪装内容）
  background_worker.h  单工作线程 + 至多一个待处理任务的采样器
tests/
  game_model_tests.cpp 逻辑层单元测试
tools/
  probe.ps1            调试助手：抓取任务栏控件/弹窗截图、发按键、切换模式
  generate_app_icon.ps1
```

### 调试

```powershell
# 查看控件 / 弹窗句柄
powershell -ExecutionPolicy Bypass -File tools/probe.ps1 -Action info

# 切换模式 / 打开弹窗
powershell -ExecutionPolicy Bypass -File tools/probe.ps1 -Action toggle
powershell -ExecutionPolicy Bypass -File tools/probe.ps1 -Action open

# 给弹窗发按键（十进制 VK：49='1'，40=Down，13=Enter）
powershell -ExecutionPolicy Bypass -File tools/probe.ps1 -Action key -Out 49

# 抓图（控件或弹窗）
powershell -ExecutionPolicy Bypass -File tools/probe.ps1 -Action shot -Out shot.png
```

> `probe.ps1` 会先把自己设为 DPI 感知，否则 150% 缩放下拿到的窗口尺寸会被系统虚拟化。

---

## 已知限制

- 只做了单机。联网 / 账号 / 云存档 / 异步社交尚未实现。
- 任务栏嵌入依赖 `Shell_TrayWnd` / `Shell_SecondaryTrayWnd` 的子窗口机制，
  Windows 大版本更新后可能需要跟着调整（参考 `taskbar_embedder.cpp`）。
- 弹窗是自绘的，没有无障碍（UIA）支持，屏幕阅读器读不到内容。
