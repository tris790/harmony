#!/bin/bash

# Harmony Build Script
# Usage: ./build.sh [run|test|install]

mkdir -p build

# Embed assets (font, etc.) into C arrays
# This must run before compilation to generate embedded_assets.c
if [ -f "scripts/embed_assets.sh" ]; then
    ./scripts/embed_assets.sh 2>/dev/null || ./scripts/embed_assets.sh
fi

# Compiler Flags
# -g: Debug info
# -O0: No optimization (for debugging) - switch to -O2 or -O3 for release
# -Wall -Wextra: Warnings
# -Wno-unused-function: Common in single translation unit builds
FLAGS="-g -O0 -Wall -Wextra -Wno-unused-function"

# Libraries
# FFmpeg, Wayland, PipeWire, DBus, Opus
LIBS="-lavcodec -lavformat -lavutil -lswscale -lwayland-client -lwayland-cursor -lwayland-egl -lEGL -lGL -lxkbcommon -lm -lopus -lpthread $(pkg-config --libs libpipewire-0.3 dbus-1)"

# Includes
INCLUDES="-Isrc $(pkg-config --cflags libpipewire-0.3 dbus-1 opus)"

# Wayland Protocols
PROTO_DIR="/usr/share/wayland-protocols/stable/xdg-shell"
GEN_DIR="src/platform/generated"
mkdir -p $GEN_DIR

# Generate XDG Shell protocol if needed
if [ ! -f "$GEN_DIR/xdg-shell-protocol.c" ]; then
    wayland-scanner private-code $PROTO_DIR/xdg-shell.xml $GEN_DIR/xdg-shell-protocol.c 2>/dev/null
    wayland-scanner client-header $PROTO_DIR/xdg-shell.xml $GEN_DIR/xdg-shell-client-protocol.h 2>/dev/null
fi

DECORATION_PROTO="/usr/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
if [ ! -f "$GEN_DIR/xdg-decoration-protocol.c" ]; then
    wayland-scanner private-code $DECORATION_PROTO $GEN_DIR/xdg-decoration-protocol.c 2>/dev/null
    wayland-scanner client-header $DECORATION_PROTO $GEN_DIR/xdg-decoration-client-protocol.h 2>/dev/null
fi

# Source Files
# We use a Unity Build (Single Translation Unit) approach for fast builds
# main.c includes everything else
SOURCES="src/main.c src/platform/generated/xdg-shell-protocol.c src/platform/generated/xdg-decoration-protocol.c src/platform/generated/embedded_assets.c src/platform/linux_threading.c src/platform/linux_wayland.c src/platform/linux_portal.c src/platform/capture_pipewire.c src/platform/audio_pipewire.c src/platform/config_linux.c src/codec/codec_ffmpeg.c src/codec/codec_ffmpeg_decode.c src/codec/audio_opus.c src/net/network_udp.c src/net/websocket.c src/net/aes.c src/ui/render_gl.c src/ui/ui_simple.c"

# Build
echo "Building Harmony..."
BUILD_OUTPUT=$(gcc $FLAGS $INCLUDES $SOURCES -o build/harmony $LIBS 2>&1)
BUILD_STATUS=$?
echo "$BUILD_OUTPUT" | grep -E '(error:|warning:)' || true

if [ $BUILD_STATUS -eq 0 ]; then
    echo "✓ Build complete"
    
    # Install desktop file after successful build
    # This allows launching from start menu without manual setup
    if [ -f "scripts/install_desktop_file.sh" ]; then
        ./scripts/install_desktop_file.sh "$(pwd)/build/harmony" >/dev/null 2>&1
        echo "✓ Desktop integration installed"
    fi
    echo ""
    echo "Launch: ./build/harmony"
    
    if [ "$1" == "run" ]; then
        # Running Harmony
        ./build/harmony
    elif [ "$1" == "test" ]; then
        TEST_FLAGS="$FLAGS -fprofile-arcs -ftest-coverage"
        
        echo "Running tests..."
        gcc $TEST_FLAGS $INCLUDES tests/test_codec_runner.c -o build/test_codec $LIBS 2>/dev/null && ./build/test_codec
        gcc $TEST_FLAGS $INCLUDES tests/test_net_runner.c -o build/test_net $LIBS 2>/dev/null && ./build/test_net
        echo "✓ Tests complete"
    fi
else
    echo "✗ Build failed"
fi
