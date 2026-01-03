# Harmony

Harmony is a high-performance, handmade screen-sharing application for Linux (Wayland), written in C. It is designed for low-latency peer-to-peer (P2P) streaming using custom UDP transport and FFmpeg.

## Features

- **Low Latency**: Custom UDP protocol with fragmentation/reassembly.
- **UDP Punchhole**: Built-in NAT traversal using STUN/TURN, runs on port 9999.
- **Wayland Native**: Built from scratch for Wayland using XDG Desktop Portal for screencasting.
- **Audio Support**: High-quality audio capture via PipeWire and encoding with Opus. Support desktop audio or specific applications.
- **Configurable**: Persistent settings in ~/.config/harmony/config.json.

## Requirements

Ensure you have the following development libraries installed:

- **Video**: `libavcodec`, `libavformat`, `libavutil`, `libswscale` (FFmpeg)
- **Audio**: `libopus`, `libpipewire-0.3`
- **Display**: `libwayland-client`, `libwayland-cursor`, `libwayland-egl`, `libEGL`, `libGL`, `libxkbcommon`
- **System**: `dbus-1`
- **Tools**: `gcc`, `pkg-config`, `wayland-scanner`, `wayland-protocols`

## How to Build

The project uses a simple shell script for building.

```bash
# Build and run the application
./build.sh run
```

## How to Use

1. **Launch**: Start the application by running `./build/harmony`.
2. **Host**: In the UI, select "Host". A system dialog (XDG Portal) will appear to let you select a screen or window to share.
3. **Connect**: Enter the IP address of the peer you wish to connect to.
4. **Settings**: Use the in-app dropdowns to select audio sources and adjust streaming parameters like bitrate.

