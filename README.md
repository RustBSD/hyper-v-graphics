# hypervgfx — Hyper-V 合成显卡驱动（FreeBSD）

让 FreeBSD 虚拟机直接使用 Hyper-V 合成显示设备（Synthetic Video / SYNTHVID），
把 KDE Plasma 桌面显示到宿主机的 Hyper-V 控制台（vmconnect）里——
不再需要 xf86-video-dummy + x11vnc 的绕行方案。

![Hyper-V 控制台中的 KDE Plasma](docs/hyperv-console-plasma.png)

图中为 Windows 11 宿主机的虚拟机连接窗口，显示运行 hypervgfx 的
FreeBSD 15.1 虚拟机桌面（1440x900）。

## 特性

- 绑定 vmbus 合成显示设备（`HV_SYNTHVID_GUID`），实现完整的 SYNTHVID 协议
  （版本协商、VRAM 定位、显示状态上报、分辨率协商、脏区通知、指针隐藏）
- **Guest 主动切换分辨率**：`SITUATION_UPDATE` 让宿主重配虚拟显示器，
  支持宿主报告的 13 档模式（640x480 ～ 1920x1080，受 8MB 显存窗口限制，
  1920x1080 是上限）
- fbdev 前端：注册 `/dev/fb0`，Xorg 配合 `xf86-video-scfb` 驱动直接写屏
- 控制台集成：vt 自动从 efifb 切到通用 fb 后端，文本控制台与 X 共用帧缓冲
- 周期性全屏 DIRT 刷新保证画面实时更新（可调频率或关闭）

## 工作原理

```
KDE Plasma / Xorg (xf86-video-scfb)
    │  /dev/fb0 (fbd + fb_info)
    ▼
hypervgfx.ko  ── vt 控制台（同一帧缓冲）
    │  SYNTHVID 协议 v3.5 (vmbus 通道)
    ▼
Hyper-V 宿主机 ──► Hyper-V 控制台窗口 (vmconnect)
```

显存复用 Hyper-V 在客户机物理地址空间预留的帧缓冲窗口
（Gen2 虚拟机的 EFI GOP 帧缓冲，本机为 8MB @ 0xf8000000）。
驱动通过 `VRAM_LOCATION` 消息把窗口 GPA 告知宿主，
之后宿主按 `SITUATION_UPDATE` 的几何参数解释帧缓冲内容，
并按 `DIRT` 消息重绘脏区。

## 构建

```sh
cd hypervgfx
make            # 生成 hypervgfx.ko
```

需要内核源码树（`/usr/src/sys`，FreeBSD 15.x）。
驱动依赖内核自带的 `vmbus`（Hyper-V 必备组件）。

## 安装与配置

```sh
cp hypervgfx.ko /boot/modules/
```

`/boot/loader.conf`：

```sh
hw.hypervgfx.mode="1440x900"   # attach 时切换到的分辨率（可选）
```

`/etc/rc.conf`：

```sh
kld_list="${kld_list} hypervgfx"
```

Xorg（`/usr/local/etc/X11/xorg.conf.d/20-hypervgfx-scfb.conf`）：
Device 用 `Driver "scfb"`，Screen 的 Modes/Virtual 与
`hw.hypervgfx.mode` 保持一致（scfb 只认 X 启动时的帧缓冲几何）。

## 运行时控制（sysctl）

| 节点 | 说明 |
|---|---|
| `hw.hypervgfx.status` | 协议版本、当前模式、帧缓冲信息 |
| `hw.hypervgfx.set_mode` | 运行时切换分辨率，如 `sysctl hw.hypervgfx.set_mode=1600x1200` |
| `hw.hypervgfx.refresh_hz` | 全屏 DIRT 刷新率（默认 15，0 关闭） |
| `hw.hypervgfx.test_pattern` | 画四分块测试图案（排查显示问题用） |

注意：切换分辨率或排查显示异常前请先停止 X 会话
（`service plasma_headless stop` 或等效的 display manager 停止命令），
否则旧 stride 的写入会造成画面错位的假象。

## 已知限制

- 显存窗口固定 8MB（Gen2 虚拟机由 Hyper-V 预留），
  1920x1080 是能放下的最大模式
- 无 GPU 命令通道，3D 只能走 Mesa llvmpipe 软件渲染
- 画面刷新靠全屏 DIRT 定时推送，未实现细粒度 damage tracking
- DRM/KMS 前端（`/dev/dri`、Wayland）尚在规划中，目前走 fbdev 路径

## 路线图

- [x] 阶段 1：SYNTHVID 协议核心（协商 / VRAM / 查询）
- [x] 阶段 2：fbdev 前端 + scfb Xorg + DIRT 刷新
- [x] 阶段 3a：guest 主动分辨率切换
- [ ] 阶段 3b：DRM/KMS 前端（`/dev/dri/card0`、modesetting、Wayland）
- [ ] 细粒度 damage tracking（替代全屏刷新）

## 环境

开发并验证于：FreeBSD 15.1-RELEASE amd64，宿主机 Windows 11 24H2
（Hyper-V 10.0.26100），KDE Plasma 6 + Xorg 21.1 + xf86-video-scfb。

## 许可证

BSD 2-Clause，与 FreeBSD 相同。见 [LICENSE](LICENSE)。
