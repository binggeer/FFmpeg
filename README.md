# ffmpeg.dll

面向 **Windows 32 位** 的 FFmpeg H.264 编解码动态库，导出 `FF_*` 接口（`__stdcall`），供 **易语言** 或其它语言通过 `.DLL 命令` / `LoadLibrary` 调用。典型场景：配合 DX 截屏纹理做实时编码、解码预览、录制 MP4、本地文件播放。

> **平台**：仅 **Win32 (x86)**。工程含 x64 配置，但易语言与现有 DX 纹理路径按 32 位设计，请使用 **Release | Win32** 构建。

---

## 功能概览

| 模块 | 说明 |
|------|------|
| 编码 | BGRA 字节集 / D3D11 纹理 → H.264；硬件优先（NVENC → AMF → QSV → MF → libx264） |
| 解码 | H.264 → BGRA，供窗口渲染 |
| 一步处理 | `FF_ProcessFrame_D3D11`：截屏纹理 → 编码 → 解码回 BGRA（低延迟预览） |
| 录制 | MP4（H.264 视频 + 可选 AAC 音频，交错复用） |
| 播放 | 打开本地文件，按 PTS 节流读 BGRA 帧 |

### 三种常用路径（易语言侧）

| 方式 | 调用 | 特点 |
|------|------|------|
| 1 | `编码_D3D纹理` + `解码` | 编码与解码分离，便于拿 H.264 包 |
| 2 | `编码_D3D纹理_解码` | 单帧 D3D 纹理进、BGRA 出，适合实时预览 |
| 3 | `编码_自字节集` + `解码` | 走字节集截屏，较慢，兼容无纹理路径 |

---

## 仓库内容

```
├── ffmpeg_api.h      # C 导出声明
├── ffmpeg.def        # 导出符号（易语言要求 FF_* 公开名）
├── ff_*.cpp / .h     # 会话、编码、解码、录制、播放、工具
├── FFmpeg.vcxproj    # VS 工程
├── FFmpeg.slnx
└── src/              # 易语言封装参考（ffmpeg.txt、.DLL声明.txt 等）
```

**不包含** FFmpeg 官方预编译包（GPL shared），需自行下载后放到工程根目录（见下文）。

---

## 环境要求

- Windows 10+
- Visual Studio 2022（或带 **v145** 工具集的 VS），组件：**使用 C++ 的桌面开发**
- 磁盘：预留约 500MB 用于 FFmpeg SDK 解压目录

---

## 获取 FFmpeg SDK

1. 打开 [FFmpeg-Builds-Win32 releases](https://github.com/sudo-nautilus/FFmpeg-Builds-Win32/releases)（或 README 中易语言注释里的同类地址）。
2. 下载 **`ffmpeg-n6.0-latest-win32-gpl-shared-6.0.zip`**（须为 **win32 gpl shared 6.0**）。
3. 解压到仓库根目录，使路径为：

```
<仓库根>/
  ffmpeg-n6.0-latest-win32-gpl-shared-6.0/
    bin/      # *.dll
    include/
    lib/
```

工程已通过 `FFmpegRoot` 指向该目录（见 `FFmpeg.vcxproj`），无需改包含路径。

---

## 编译

1. 用 Visual Studio 打开 `FFmpeg.slnx` 或 `FFmpeg.vcxproj`。
2. 配置选择 **Release**，平台选择 **Win32**。
3. 生成解决方案。

成功后，在输出目录（一般为 `Release\`）得到：

- `ffmpeg.dll` — 本工程产物
- `avcodec-60.dll`、`avformat-60.dll`、`avutil-58.dll`、`swscale-7.dll`、`swresample-4.dll` 等 — 由生成后事件从 SDK `bin` 复制

部署到易语言程序时，请将 **上述 DLL 放在同一目录**（或系统 PATH 可见处）。

---

## 易语言集成

参考 `src/`：

| 文件 | 用途 |
|------|------|
| `ffmpeg.txt` | 类封装：`初始化`、`编码_D3D纹理`、`解码`、`播放_*` 等 |
| `.DLL声明.txt` | 与 `ffmpeg.def` 一致的 DLL 命令声明 |

初始化示例（参数含义见 `ffmpeg.txt`）：

```text
ff.初始化 (宽度, 高度, , 60, 2500)
' 空参编码方式 = 自动硬编，失败再 CPU
' 帧率、码率单位 KB/s
```

销毁时务必调用 `ff.销毁()`（内部 `FF_PlayClose` → `FF_RecordEnd` → `FF_Free`），避免编码器 / D3D 缓存残留。

---

## 主要 C API

完整列表见 `ffmpeg_api.h`，导出名见 `ffmpeg.def`。

| 函数 | 说明 |
|------|------|
| `FF_Init` | 创建会话，返回 handle（0 失败） |
| `FF_Free` | 释放会话及所有资源 |
| `FF_GetLastErrorGBK` | 最近错误（GBK 文本） |
| `FF_GetEncoderTypeGBK` | 当前编码器名称 |
| `FF_GetRealtimeFps` / `FF_GetEncodeBitrate` | 统计 |
| `FF_EncodeFrame_BGRA` / `FF_EncodeFrame_D3D11` | 编码 |
| `FF_ProcessFrame_D3D11` | 纹理 → H.264 → BGRA 一步 |
| `FF_DecodePacket_RenderBGRA` | 解码到 BGRA |
| `FF_RecordBeginEx` | 第三参：`0` 仅视频，非 `0` 录环回 AAC |
| `FF_RecordHasAudio` | 录制中是否已创建音频轨（1/0） |

| `FF_RecordFrame_D3D11` / `FF_RecordFrame_BGRA` | 每帧同时写视频+对应时长音频 |
| `FF_RecordEnd` | 结束录制 |
| `FF_PlayOpenEx` | 第三参：`0` 仅画面，非 `0` 扬声器播放音轨 |
| `FF_PlayOpen` | 等同 `PlayOpenEx(..., 1)` |
| `FF_PlayReadBGRA` / `FF_PlayClose` 等 | 播放 |

字符串路径、错误信息均为 **GBK**。

---

## 许可证说明

- 本仓库 **源码**：请按你方仓库选择的许可证发布（若未指定，建议在 GitHub 仓库设置中补充 License）。
- **FFmpeg 预编译包** 为 **GPL**（shared 构建）。分发 `ffmpeg.dll` 及随附的 `avcodec` 等 DLL 时，请遵守 GPL 义务（提供源码或对应说明、许可证文本等）。详见 SDK 内 `LICENSE.txt`。

---

## 常见问题

**Q: 初始化失败 / 找不到编码器？**  
A: 确认 SDK 目录名正确、已选 Win32 Release，且 GPU 驱动支持对应硬编；可传 `use_cpu=1` 强制 libx264。

**Q: 设置 60 帧但实测只有三十多帧？**  
A: 多为单帧编解码耗时或显示器刷新限制；高分辨率可尝试提高码率或使用方式 2。DLL 内对目标帧率有节流。

**Q: GitHub 克隆后无法编译？**  
A: 必须自行下载并解压 FFmpeg SDK（`.gitignore` 已排除该目录）。

---

## 相关链接

- [FFmpeg](https://ffmpeg.org/)
- [FFmpeg-Builds-Win32](https://github.com/sudo-nautilus/FFmpeg-Builds-Win32/releases)
