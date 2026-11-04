# 星际领主（Stellar Lord）

> **星际领主 / Stellar Lord**
>
> 联机入口为「加入时空」；启动时可选择服务器或客户端。
> 世界观、剧本与完整设计见 [`docs/README.md`](docs/README.md)。
> 本 README 描述当前实现与运行方式。

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

产物：`build/Release/stellar_lord_taskbar.exe`、`build/Release/stellar_lord_server.exe`

跑测试（纯逻辑层，不依赖 Win32）：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

---

## 发版（Windows CI）

仓库自带一条 **仅 Windows** 的构建 / 发版流水线：
[`.github/workflows/windows-release.yml`](.github/workflows/windows-release.yml)。
它固定在 `windows-2022` + Visual Studio 2022 x64 上运行，不生成 Linux 或 macOS 包。

触发方式：

| 触发 | 行为 |
| --- | --- |
| 推送 `v*` 标签 | 构建 → `ctest` → 无窗口 `--self-test` → 打包 → 创建 GitHub Release |
| 手动 `workflow_dispatch` | 同样构建、测试、打包，只上传 Actions artifact，不发 Release |

发版前先把 `CMakeLists.txt` 里的 `project(... VERSION x.y.z)` 改成目标版本，标签必须严格等于 `vx.y.z`，否则流水线会拒绝发布。

```powershell
# 发布 v0.1.0
git tag -a v0.1.0 -m "Stellar Lord v0.1.0"
git push origin main
git push origin v0.1.0
```

Release 附件为 `StellarLord-<版本>-windows-x64.zip` 和同名 `.sha256`，zip 内包含：

- `stellar_lord_taskbar.exe`（客户端 / 内嵌主机）
- `stellar_lord_server.exe`（独立服务端）
- `README.md`、`docs/`、`VERSION.txt`

本地也可以手动打包：

```powershell
./tools/package_release.ps1 -Version 0.1.0
```

> 发布检查：协议版本、存档版本、文档里的 `vN`、`CMakeLists.txt` 版本必须同步修改；`VERSION.txt` 会记录本次构建的协议 / 存档版本。

---

## 运行

直接双击 `stellar_lord_taskbar.exe`。启动时会询问这台电脑以「服务器」还是「客户端」身份进入：

- 服务器：在这台电脑上创建时空，自动生成一条可复制的邀请链接。
- 客户端：打开加入窗口，粘贴朋友发来的邀请链接即可加入。

也可以绕过启动窗口，直接用命令行参数启动：

```powershell
# 作为主机启动（内嵌服务器，自动建房、开局）
stellar_lord_taskbar.exe --server

# 作为客户端启动，并预填邀请链接
stellar_lord_taskbar.exe --client --invite=stellar://join?host=192.168.1.5&port=8787&room=ABCD

# 指定端口
stellar_lord_taskbar.exe --server --port=9000

# 无窗口联机自检：不创建任何 GUI，跑完写 selftest_report.txt，退出码 0/1
stellar_lord_taskbar.exe --self-test
```

> 首次运行会在 exe 同目录生成 `config.json`。昵称、玩家 ID、服务器地址会自动保存。
> 因此不要放在 `C:\Program Files` 之类无写权限的目录，建议单独放一个文件夹。

---

## 联机

- 邀请链接格式：`stellar://join?host=192.168.1.5&port=8787&room=ABCD`。
- 主机在联机页或主机面板点「一键复制」，把链接发给朋友。
- 客户端点「粘贴并加入」即可；也兼容 `http://host:port/join/ABCD` 和纯房间码。
- 服务器会校验双方的协议版本，版本不一致直接拒绝联机，避免不同版本互相写坏状态。
- 主动退出后，同一玩家 ID 在 **30 分钟内**重新加入同一个时空会恢复原帝国；房主退出不会立刻关闭内嵌服务器。
- 对局进度只存在服务端内存里，不写客户端文件；关闭服务端进程后房间消失。

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
| `1` ~ `9` | 切换「发展 / 科研 / 军事 / 星图 / 贸易 / 远征 / 事件 / 联机 / 殖民」 |
| `←` `→` | 切换标签 |
| `↑` `↓` | 选择行 |
| `Enter` / `Space` | 执行选中行 |
| `Q` / `E` | 对双选项行选左 / 右选项（事件、劫掠/占领） |
| `Esc` | 关闭弹窗 |
| 滚轮 | 切换标签 |

弹窗里的行都是可点的：点一下就是执行（升级设施、研究科技、造船、殖民、殖民地定位、区域建设、处理事件、建造奇观、联机操作）。联机页支持一键复制邀请链接、开启本机服务器、粘贴链接加入。

---

## 玩法

**目标**：从平民起步，把首都、殖民地、舰队、科技和奇观堆起来，在霸业值上压过其他领主。

**资源与设施**

- 基础资源：能源、矿物、食物、合金、科研、人口。
- 六类设施：电站、矿场、农场、工厂、实验室、军营。
- 设施升级消耗资源并逐级涨价；实验室耗电，军营耗粮，工厂吃矿物。
- 人口受住房上限约束；人口不足会拖慢生产和造船。
- 士气影响全局产能：高士气有加成，低士气会明显减产。事件、战败和路线选择都会改变士气。

**殖民地定位**

- 每座殖民地可以选一种定位：均衡、能源、矿业、农业、工业、科研、贸易、要塞。
- 第一次选择免费；已经定过位的殖民地改定位要花 10 影响力。
- 定位按殖民地等级叠加：
  - 能源 / 矿业 / 农业 / 工业 / 科研世界：对应资源产出 +9%/级，并有一个小代价。
  - 贸易港：市场卖价 +2%/级，影响力 +4%/级。
  - 要塞世界：舰队上限 +4/级。
  - 均衡：无加成，无惩罚。
- 星图页的殖民地行有「升级」和「转为下一种定位」两个按钮；联机时由服务端验证影响力。

**殖民地特质**

- 每座新殖民地会随机获得 1 个特质；殖民、占领中立星域时生成，占领玩家殖民地时随殖民地转移。
- 富矿：矿物 +30%。
- 沃土：食物 +30%。
- 科研遗迹：科研 +20%，事件更频繁。
- 潮汐锁定：能源 +20%，人口上限 -10%。
- 有毒大气：合金 +20%，人口增长 -20%。
- 古代轨道：防御 +30%，受到劫掠 / 占领时生效。
- 特质与定位独立叠加，星图页会同时显示两者。

**殖民地区域**

- 每座殖民地按等级解锁区域槽位：1 级 1 个，最多 5 个；同一类型区域每座殖民地只能建一个。
- 区域最高 3 级，建造 1 级消耗 55 矿物 + 18 合金，升级成本逐级翻倍。
- 能源 / 矿业 / 农业 / 工业 / 科研区：对应产出 +20%/级。
- 军事区：舰队上限 +5/级，防御 +15%/级。
- 文化区：影响力 +2/分钟/级。
- 港口区：市场卖价 +5%/级，买价 -3%/级。
- 按 `9` 打开「殖民」页，可以切换殖民地并建造 / 升级区域。

**贸易、星际代币与稀有资源**

- 新增**星际代币（ST）**：不能靠建筑生产，只能通过卖资源、合同、掠夺、远征和事件获得。
- 新增**影响力（INF）**：由领土和行政随时间产出，合同、救援和外交事件也会给；用于殖民地改定位、奇观、殖民和终局评分。
- 贸易页可以按实时价格买卖能源 / 矿物 / 食物 / 合金；价格每 15 分钟刷新一次，信誉越高卖价越好、买价越低。
- 每 15 分钟刷新 3 份**贸易合同**：交付指定资源，换 ST、影响力和信誉。
- **黑市**与市场同步为每位玩家刷新 3 件商品：遗物、星核、数据核心、雇佣兵、科研缓存或政治献金。每件库存有限。
- 稀有资源**遗物 REL / 星核 COR / 数据核心 DATA** 只能通过远征、黑市、事件或掠夺获得，并计入霸业值。

**远征**

- 远征页可同时派出最多 3 支远征队，从现有舰队抽船。
- 目的地：未探索扇区、遗迹带、深空异常、漂流方舟；风险、时长和产出各不相同。
- 每支远征队可选「谨慎」或「深入」：谨慎收益 -20%、损失 -30%；深入收益 +30%、损失 +50%。
- 远征结果在出发时就已经确定，返航时才揭示；不能靠读档改结果。
- 返航会带回稀有资源、ST、科研和影响力，也可能损失舰船、士气与信誉。返航报告保留在远征页，可手动清除。

**指令卡**

- 每 15 分钟抽 3 张指令卡，120 秒内选 1 张；不选就消失，不会打断挂机。
- 指令卡提供的是一次性收益与代价，例如紧急征召、科研突破、殖民补贴、紧急贷款、人才引进、外交斡旋、黑市采购、静默监听。

**时代**

- 单局按时间推进：苏醒 → 第一时代：立足 → 第二时代：扩张 → 第三时代：审计 → 结算。
- 时代只影响叙事、事件氛围和终局评分，不强制卡进度。事件页会显示当前时代和「阿刻 / 伊莱亚斯 / 赫卡忒 / 静默者」的编年史片段。

**治国路线（每局一次）**

| 路线 | 特点 |
| --- | --- |
| 铁砧工业 | 合金 +30%，能源更吃紧 |
| 星海教团 | 科研 +35%，能源产出略降 |
| 拓荒议会 | 人口上限 +25%，增长 +15%，初始士气 +8 |
| 自由商盟 | 掠夺收益 +25%，造船便宜 15%，合金 -5% |

**事件卡**

- 游戏过程中会不断出现事件，每次提供两个有代价的选项。
- 事件最多排队 4 条；超过上限时最旧的一条按保守选项自动结算。
- 选项会影响资源、人口、舰队、科研、士气和稀有资源。
- 新增剧情事件：回声信号、方舟求救、静默潮汐，分别对应数据核心、人口/影响力和星核。

**奇观**

- 戴森环：能源产出 +100%。
- 知识方舟：科研 +100%，科技成本 -25%（需要 300 科研）。
- 轨道船坞：舰队上限 +50%，训练速度 +50%。

**舰队与战争**

- 军营自动训练舰队，造船消耗合金、食物和人口。
- 可攻击中立区、富矿带和静默者前哨；富矿带偏矿物，前哨偏科研但有士气代价。
- 对其他玩家可以「劫掠」或「占领」：
  - 劫掠抢资源、科研、最多 10% 国库 ST，并削减对方舰队，但不会夺走殖民地。
  - 占领会尝试夺走一座殖民地，风险和损失更高，最多抢走 15% 国库 ST。
  - 对玩家动武会降低信誉；信誉影响市场报价和合同收益。

**联机对局**

- 所有人以平民身份同时开局，服务器统一结算所有操作。
- 迟到玩家按已开局时间的 75% 追赶，最多补 30 分钟。
- 主动退出后 30 分钟内用同一玩家 ID 回来，会恢复原帝国。
- 进度只存在服务端内存里，不落客户端磁盘；服务端关闭即结束。

**全房间审计压力**

- 每个时空有一个全房间共享的**审计压力（0–100）**，开局为 15。
- 压力随房间时间和活跃人数上升；黑市交易、远征、奇观、中立战斗和玩家冲突会额外增加。
- 压力到达 40 / 65 / 85 时触发三档共享危机：采样、潮汐、归档。所有成员同时受影响，不会直接淘汰任何人。
- 事件页可以花 **1 数据核心 + 40 能量**降 15 压力，或花 **30 能量**做静默广播降 5 压力。降低的是全房间压力。
- 联机页和事件页都会显示当前压力与危机等级；这是目前唯一的全房间共享机制。

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
  "auto_start": false,
  "server_host": "127.0.0.1",
  "server_port": 8787,
  "player_id": "0123456789abcdef",
  "nickname": "Lord"
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
| `server_host` / `server_port` | 客户端连接的服务器地址；本机主机时自动写回 `127.0.0.1` |
| `player_id` | 本地生成的唯一玩家 ID；退出后用同一 ID 重连可恢复联机存档 |
| `nickname` | 上次使用的昵称 |

对局进度不写本地文件；联机状态由服务端内存保存。

---

## 目录结构

```
.github/
  workflows/windows-release.yml  Windows CI / 发版流水线
src/
  main.cpp             Win32 UI、启动角色选择、主机/加入窗口、任务栏控件与弹窗
  game_model.h/.cpp    纯逻辑层：资源、设施、科技、舰队、路线、事件、奇观、战斗、序列化
  net_client.h/.cpp    极简 HTTP 客户端与邀请链接用的 URL 编码
  app_config.h/.cpp    配置读写（昵称、玩家 ID、服务器地址等）
  taskbar_embedder.*   任务栏嵌入（Win10 / Win11）
  system_metrics.*     系统指标采样与格式化
  process_monitor.*    进程列表采样（监视器模式的伪装内容）
  background_worker.h  单工作线程 + 至多一个待处理任务的采样器
server/
  server_core.h/.cpp   可复用的内存房间服务器核心（独立服务端和客户端主机模式共用）
  main.cpp             独立服务端入口 stellar_lord_server.exe
tests/
  game_model_tests.cpp 逻辑层单元测试
tools/
  probe.ps1            调试助手：抓取任务栏控件/弹窗截图、发按键、切换模式
  generate_app_icon.ps1
  package_release.ps1  收集 Release 产物、生成 VERSION.txt / zip / SHA-256
```

### 调试

```powershell
# 无窗口联机自检：建房、加入、开局、操作、退出回档、迟到追赶、版本拒绝
build/Release/stellar_lord_taskbar.exe --self-test
Get-Content selftest_report.txt

# 纯逻辑层测试
ctest --test-dir build -C Release --output-on-failure
```

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

- 联机数据只存在服务端内存，关闭服务端进程后房间消失；没有账号、云存档和跨房间排行榜。
- 大厅只扫描当前配置的服务器；不同服务器之间不互通。
- 版本校验是协议级硬拒绝：双方必须使用同一构建，旧客户端/旧服务端都会提示版本不一致。
- 任务栏嵌入依赖 `Shell_TrayWnd` / `Shell_SecondaryTrayWnd` 的子窗口机制，
  Windows 大版本更新后可能需要跟着调整（参考 `taskbar_embedder.cpp`）。
- 弹窗是自绘的，没有无障碍（UIA）支持，屏幕阅读器读不到内容。
