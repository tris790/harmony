#!/bin/bash

# Harmony Build Script
# Usage: ./build.sh [run|test]

mkdir -p build

# Compiler Flags
# -g: Debug info
# -O0: No optimization (for debugging) - switch to -O2 or -O3 for release
# -Wall -Wextra: Warnings
# -Wno-unused-function: Common in single translation unit builds
FLAGS="-g -O0 -Wall -Wextra -Wno-unused-function"

# Libraries
# FFmpeg, Wayland, PipeWire, DBus, Opus
LIBS="-lavcodec -lavformat -lavutil -lswscale -lwayland-client -lwayland-cursor -lwayland-egl -lEGL -lGL -lxkbcommon -lm -lopus $(pkg-config --libs libpipewire-0.3 dbus-1)"

# Includes
INCLUDES="-Isrc $(pkg-config --cflags libpipewire-0.3 dbus-1 opus)"

# Wayland Protocols
PROTO_DIR="/usr/share/wayland-protocols/stable/xdg-shell"
GEN_DIR="src/platform/generated"
mkdir -p $GEN_DIR

if [ ! -f "$GEN_DIR/xdg-shell-protocol.c" ]; then
    echo "Generating XDG Shell Protocol..."
    wayland-scanner private-code $PROTO_DIR/xdg-shell.xml $GEN_DIR/xdg-shell-protocol.c
    wayland-scanner client-header $PROTO_DIR/xdg-shell.xml $GEN_DIR/xdg-shell-client-protocol.h
fi

DECORATION_PROTO="/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
if [ ! -f "$GEN_DIR/xdg-decoration-protocol.c" ]; then
    echo "Generating XDG Decoration Protocol..."
    wayland-scanner private-code $DECORATION_PROTO $GEN_DIR/xdg-decoration-protocol.c
    wayland-scanner client-header $DECORATION_PROTO $GEN_DIR/xdg-decoration-client-protocol.h
fi

# Source Files
# We use a Unity Build (Single Translation Unit) approach for fast builds
# main.c includes everything else
SOURCES="src/main.c src/platform/generated/xdg-shell-protocol.c src/platform/generated/xdg-decoration-protocol.c src/platform/linux_wayland.c src/platform/linux_portal.c src/platform/capture_pipewire.c src/platform/audio_pipewire.c src/platform/config_linux.c src/codec/codec_ffmpeg.c src/codec/codec_ffmpeg_decode.c src/codec/audio_opus.c src/net/network_udp.c src/net/websocket.c src/net/aes.c src/ui/render_gl.c src/ui/ui_simple.c"

echo "Building Harmony..."
gcc $FLAGS $INCLUDES $SOURCES -o build/harmony $LIBS

if [ $? -eq 0 ]; then
    echo "Build Successful."
    if [ "$1" == "run" ]; then
        echo "Running Harmony..."
        ./build/harmony
    elif [ "$1" == "test" ]; then
        echo "Building and Running Tests with Coverage..."
        TEST_FLAGS="$FLAGS -fprofile-arcs -ftest-coverage"
        
        echo "Running Codec Test..."
        gcc $TEST_FLAGS $INCLUDES tests/test_codec_runner.c -o build/test_codec $LIBS
        ./build/test_codec
        
        echo -e "\nRunning Network Test..."
        gcc $TEST_FLAGS $INCLUDES tests/test_net_runner.c -o build/test_net $LIBS
        ./build/test_net
    fi
else
    echo "Build Failed."
fi
