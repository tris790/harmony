# Harmony

Harmony is a high-performance, handmade screen-sharing application for Linux (Wayland), written in C. It is designed for low-latency peer-to-peer (P2P) streaming using custom UDP transport and FFmpeg.

## Features

- **Low Latency**: Custom UDP protocol with fragmentation/reassembly.
- **UDP Punchhole**: Built-in NAT traversal using STUN/TURN, runs on port 9999.
- **Wayland Native**: Built from scratch for Wayland using XDG Desktop Portal for screencasting.
- **Audio Support**: High-quality audio capture via PipeWire and encoding with Opus. Support desktop audio or specific applications.
- **Configurable**: Persistent settings in ~/.config/harmony/config.txt.
- **Desktop Integration**: Automatically installs to application menu on build.

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
# Build the application
./build.sh

./build.sh run
```

## How to Use

### Method 1: Launch from Application Menu (Recommended)
1. Build the application with `./build.sh`
2. Open your application menu/start menu
3. Search for "Harmony Screen Share"
4. Launch the application

The desktop entry includes quick actions:
- **Start as Host**: Launch directly in host mode
- **Start as Viewer**: Launch directly in viewer mode

### Method 2: Command Line
1. **Host**: Run `./build/harmony` or `./build/harmony host`
2. **Viewer**: Run `./build/harmony viewer`
3. In the UI, select "Host". A system dialog (XDG Portal) will appear to let you select a screen or window to share.
4. Enter the IP address of the peer you wish to connect to.
5. Use the in-app dropdowns to select audio sources and adjust streaming parameters like bitrate.

