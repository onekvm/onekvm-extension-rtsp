# OneKVM RTSP 扩展

[English](README.md) | 简体中文

通过独立进程的协议扩展，将 OneKVM 硬件编码后的视频流提供给标准 RTSP
客户端。运行时直接消费 OneKVM 的编码流，支持 H.264 和 H.265 两种模式。

> **构建说明：** 本仓库只包含扩展源码。正式软件包由 `onekvm-distro` 中的
> OpenEmbedded（OE）配方统一构建；本仓库不提供独立的正式构建流程。

## 功能

- 提供 H.264 和 H.265 RTSP 视频
- 可配置监听地址、TCP 端口和 RTSP 路径
- 支持可选的用户名和密码鉴权
- 可控制帧率、画质预算、最大码率、QP 和 GOP
- 直接使用 OneKVM 的编码视频能力，不经过软件解码和重新编码

## 安装与启用

软件包名称是 `onekvm-extension-rtsp`。请通过 OneKVM 发行版的软件包或镜像
安装，然后使用插件管理器控制扩展：

```sh
onekvm-plugin-manager enable rtsp
onekvm-plugin-manager disable rtsp
```

## 配置与连接

打开 **RTSP** 扩展设置并配置服务器。默认只监听 `127.0.0.1`，端口为
`8554`，路径为 `/onekvm`。将监听地址改为客户端可访问的接口后，通过以下
地址连接：

```text
rtsp://<你的-onekvm-地址>:8554/onekvm
```

默认视频模式为 H.264，帧率上限为 60 FPS。最大码率、各项 QP 和 GOP 设为
`0` 时由编码器自动选择。画质预算用于控制当前视频消费者可使用的资源比例，
范围为 0.1 到 1.0。

RTSP 用户名和密码是可选项，但服务能从可信网络以外访问时强烈建议启用。
RTSP 不具备与 HTTPS 相同的传输安全性，远程访问时请使用私有网络或 VPN。

## 实现说明

C++20 运行时使用 Live555 提供 RTSP 传输，并订阅 OneKVM 的
`video.encoded` 能力。扩展生命周期钩子负责启动和停止服务，不需要将协议
实现加入 OneKVM Core。

## 开发

使用 CMake 构建运行时和可选协议测试：

```sh
cmake -S . -B build -DONEKVM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

开发环境需要 Live555、json-c、OpenSSL 和兼容 pthread 的线程支持。正式
依赖与打包流程由 `onekvm-distro` 定义。

## 许可证

本项目使用 GNU General Public License v3.0，完整条款见
[`LICENSE`](LICENSE)。
