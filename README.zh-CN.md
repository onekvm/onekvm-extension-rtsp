# OneKVM RTSP 扩展

[English](README.md) | 简体中文

> **构建说明：** 本仓库只包含扩展源码。扩展统一由 `onekvm-distro` 中的
> OpenEmbedded（OE）配方构建和打包；本仓库不提供独立的正式构建流程。

这是 OneKVM 的进程外 RTSP 协议支持。扩展独立维护运行时、manifest、
设置界面、测试和发布版本，并通过 OneKVM 扩展接口获取视频流。

用户可在扩展设置中配置监听地址、端口、RTSP 路径、帧率、码率、编码参数
以及可选的用户名和密码。
