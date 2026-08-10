# OneKVM RTSP Extension

English | [简体中文](README.zh-CN.md)

Expose the hardware-encoded OneKVM video stream to standard RTSP clients from
an out-of-process protocol extension. The runtime consumes OneKVM's encoded
stream directly and supports both H.264 and H.265 modes.

> **Build note:** This repository contains extension sources only. Release
> packages are built by the OpenEmbedded recipes in `onekvm-distro`; this
> repository does not provide a standalone release build.

## Features

- H.264 and H.265 RTSP video
- Configurable listen address, TCP port, and RTSP path
- Optional username and password authentication
- Frame-rate, quality-budget, maximum-bitrate, QP, and GOP controls
- Direct use of OneKVM's encoded video capability without a software
  decode/re-encode step

## Install and enable

The package name is `onekvm-extension-rtsp`. Install it through your OneKVM
distribution package or image, then manage it with the plugin manager:

```sh
onekvm-plugin-manager enable rtsp
onekvm-plugin-manager disable rtsp
```

## Configure and connect

Open the **RTSP** extension settings and configure the server. By default it
listens only on `127.0.0.1`, port `8554`, with path `/onekvm`. After changing
the bind address to one reachable by your client, connect to:

```text
rtsp://<your-onekvm-host>:8554/onekvm
```

The default video mode is H.264 at up to 60 FPS. A maximum bitrate of `0`, QP
values of `0`, and a GOP value of `0` leave those encoder choices automatic.
The quality budget scales the video resources available to this consumer from
0.1 to 1.0.

RTSP credentials are optional but strongly recommended when the service is
reachable beyond a trusted network. RTSP does not provide the same transport
security as HTTPS; use a private network or VPN for remote access.

## How it works

The C++20 runtime uses Live555 for RTSP transport and subscribes to OneKVM's
`video.encoded` capability. Extension lifecycle hooks start or stop the server
without adding protocol code to OneKVM Core.

## Development

Build the runtime and optional protocol test with CMake:

```sh
cmake -S . -B build -DONEKVM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Development requires Live555, json-c, OpenSSL, and pthread-compatible thread
support. Release dependencies and packaging are defined by `onekvm-distro`.

## License

This project is licensed under the GNU General Public License v3.0. See
[`LICENSE`](LICENSE).
