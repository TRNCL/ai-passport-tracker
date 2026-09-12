# 曲库与打包工具使用说明 (Tracker Music & Packaging Guide)

本目录存放 FoloToy AI Passport Tracker 随身听的音乐曲目数据及配置文件。

播放器支持 **ProTracker 4通道 MOD 格式** 与 **FastTracker II 多通道 XM 格式**，音乐数据存放在 Flash 的独立 `tracks` 分区（起始地址 `0x360000`，容量 4.625 MB），与核心固件程序完全分离。

---

## 快速添加新歌（无需重新编译 C 固件）

### 第一步：将音乐文件放入本目录
将下载好的 `.mod` 或 `.xm` 文件拷贝至 `main/tracks/` 目录下。

### 第二步：编辑 `tracks.json` 歌单配置文件
在 `main/tracks/tracks.json` 中添加新曲目信息：
```json
[
  {
    "file": "your_song.mod",
    "title": "Song Title",
    "artist": "Artist Name",
    "format": "MOD 4CH"
  },
  {
    "file": "another_song.xm",
    "title": "Another Song",
    "artist": "Artist Name",
    "format": "XM 10CH"
  }
]
```

### 第三步：运行打包脚本
在项目根目录下执行打包脚本：
```bash
python tools/pack_tracks.py
```
脚本会自动读取 `tracks.json`，在 `build/tracks.bin` 生成包含目录索引表（TOC）和音频数据的独立分区镜像文件。

### 第四步：仅烧录音乐分区
使用 `esptool` 将生成的 `tracks.bin` 写入设备的 `0x360000` 偏移处（仅需 2~3 秒）：
```bash
python -m esptool --chip esp32c3 -p COM3 write_flash 0x360000 build/tracks.bin
```
*(请将 `COM3` 替换为您设备的实际串口号)*

设备重启后即可直接识别并播放新歌单！

---

## 分区规格参考
- **分区名称**：`tracks`
- **起始偏移**：`0x360000`
- **分区大小**：`0x4A0000`（4,849,664 字节，约 4.625 MB）
- **受保护区域**：`0x356000 ~ 0x35A000`（原厂 CardID 分区，禁止覆盖）
