# FoloToy AI Passport Tracker Player 🎵

适用于 **FoloToy AI Passport**（ESP32-C3）的离线复古芯片音乐（Tracker MOD/XM）播放器。

基于硬件级优化，纯离线实时合成解码 4 通道 ProTracker `.mod` 与多通道 FastTracker 2 `.xm` 格式，驱动 ES8311 芯片进行高品质 I2S 音频输出。屏幕以复古绿黑终端风格实时可视化展示通道乐谱走带、触发音符、电平跳动与播放参数，并内置暗光像素屏保。

---

## 🎮 按键交互指南

设备拥有 3 个实体按键：**上键 (UP)**、**中间键 (OK)**、**下键 (DOWN)**。

| 操作 | 亮屏状态功能 | 息屏屏保状态功能 |
| :--- | :--- | :--- |
| **单击 OK 键** | 播放 / 暂停切换 | **唤醒屏幕**（恢复 100% 亮度，不打扰当前播放/暂停状态） |
| **长按 OK 键** | 进入 **5% 微光像素外星人屏保** | 保持屏保状态 |
| **单击 上键** | 音量 +5% | 音量 +5%（不唤醒屏幕） |
| **单击 下键** | 音量 -5% | 音量 -5%（不唤醒屏幕） |
| **长按 上键** | 切换至**上一首**（播放时切歌自动播放，暂停时保持暂停） | 切换至上一首（在屏保下直接切歌，不唤醒屏幕） |
| **长按 下键** | 切换至**下一首**（播放时切歌自动播放，暂停时保持暂停） | 切换至下一首（在屏保下直接切歌，不唤醒屏幕） |

---

## 💻 串口控制台命令 (115200 波特率)

通过 USB 串口连接设备，可使用功能完整的行缓冲交互控制台：

| 命令 | 简写 / 示例 | 说明 |
| :--- | :--- | :--- |
| `list` | `ls` | 列出曲库中所有曲目、大小、格式及当前正在播放的曲目 |
| `<ID>` | `0`, `3`, `6` | 输入纯数字直接切换到指定编号曲目（状态继承） |
| `play [ID]` | `play`, `play 2` | 开始播放当前曲目，或指定曲目编号并立即播放 |
| `pause` | `stop` | 暂停播放 |
| `next` | `n` | 切换下一首 |
| `prev` | `p` | 切换上一首 |
| `vol <值>` | `vol 80`, `+`, `-` | 设置或步进增减音量 (0 ~ 100) |
| `status` | `s` | 打印播放器状态快照（BPM、Speed、VU、Row、通道数等） |
| `screen` | `disp` | 手动在 亮屏 $\leftrightarrow$ 息屏（屏保）之间切换 |
| `[空格键]` | 空格 | 快速切换播放 / 暂停 |
| `help` | `?` | 显示串口控制台命令帮助 |

---

## 🎼 制作与烧录自定义曲库

本工程将**代码固件**与**曲库资源**完全解耦：
- 曲库独立烧录在 Flash 的 `0x360000` 偏移处（容量 4.625 MB）。
- **更新曲目不需要重新编译或覆盖主程序，也不会破坏设备预置的 `cardid` 保护分区**。

### 1. 准备曲目
将喜欢的 `.mod`（4 通道）或 `.xm` 音频文件放入 `main/tracks/` 目录。
推荐曲目资源站：[The Mod Archive (modarchive.org)](https://modarchive.org/)。

### 2. 编辑曲目清单
编辑 `main/tracks/tracks.json`，配置曲目信息：
```json
[
  {
    "file": "elysium.mod",
    "title": "Elysium",
    "artist": "Jugi",
    "format": "MOD 4CH"
  },
  {
    "file": "unreeeal_superhero_3.xm",
    "title": "Unreeeal Superhero 3",
    "artist": "Rez & Kenet",
    "format": "XM 6CH"
  }
]
```

### 3. 一键打包曲库
运行工程自带的打包脚本：
```bash
python tools/pack_tracks.py
```
打包成功后将在 `build/tracks.bin` 生成包含二进制 TOC 目录索引与全部音轨的合集镜像。

### 4. 独立烧录曲库到设备
无需触碰主固件，直接烧录曲库镜像至 `0x360000`：
```bash
python -m esptool --chip esp32c3 -p COM3 -b 460800 write_flash 0x360000 build/tracks.bin
```

---

## 🛠️ 编译与烧录完整工程

### 环境要求
- ESP-IDF **v5.5.3**
- Target: `esp32c3`

### 1. 编译固件
```powershell
idf.py build
```

### 2. 烧录程序
首次烧录包含 bootloader、分区表与主程序：
```powershell
idf.py -p COM3 flash
```

### 3. 合并全量量产固件 (可选)
如果需要打包为单一完整固件（用于量产或发布）：
```bash
esptool.py --chip esp32c3 merge_bin -o build/FoloToy-AI-Passport-full.bin \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/FoloToy-AI-Passport.bin \
  0x360000 build/tracks.bin
```
（注意：`0x356000` 为设备的出厂 cardid 硬件识别分区，请勿向该区域写入）。

---

## 📂 工程目录结构

```
ai-passport-tracker/
├── CMakeLists.txt              # ESP-IDF 顶层构建脚本
├── partitions.csv              # 自定义分区表 (0x360000 tracks 分区)
├── sdkconfig.defaults          # 硬件核心参数配置
├── LICENSE                     # MIT 开源协议
├── README.md                   # 本文档
├── components/
│   └── bsp/                    # 板级支持包 (屏幕/音频/按键/电源)
├── main/
│   ├── CMakeLists.txt          # 主组件构建规则
│   ├── main.c                  # 启动入口、按键与串口调度
│   ├── demo_tracker.c/.h       # 界面渲染、屏保与按键事件
│   ├── tracker_player.c/.h     # 播放控制、状态管理、I2S 驱动
│   ├── tracker_tracks.c/.h     # TOC 索引解析与 Flash 内存映射
│   ├── pocketmod.h             # ProTracker MOD 解码内核
│   ├── ibxm.c/.h               # FastTracker 2 XM / S3M 解码内核
│   └── tracks/                 # 曲目资源目录与元数据清单
│       ├── tracks.json
│       └── *.mod / *.xm
├── tools/
│   ├── pack_tracks.py          # 曲库打包工具
│   └── verify_firmware.py      # 固件完整性校验工具
└── tests/
    ├── test_tracker_logic.py   # 播放器交互逻辑测试
    └── test_tracker_player.c   # 播放器核心测试用例
```

---

## 📜 开源协议
本项目采用 [MIT License](LICENSE) 协议开源。
内置试听音轨遵循各自原作者的自由分发许可（The Mod Archive）。
