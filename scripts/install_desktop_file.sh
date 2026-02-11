#!/bin/bash
# Install desktop file and icon for Harmony
# This allows launching from the start menu in KDE and other DEs

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
ASSETS_DIR="$PROJECT_ROOT/assets"

# Get the actual install path (where the binary will be)
if [ -n "$1" ]; then
    BINARY_PATH="$1"
else
    # Default to project build directory
    BINARY_PATH="$PROJECT_ROOT/build/harmony"
fi
# Create directories
mkdir -p ~/.local/share/applications
mkdir -p ~/.local/share/icons/hicolor/256x256/apps
mkdir -p ~/.local/share/icons/hicolor/scalable/apps
mkdir -p ~/.local/share/icons/hicolor/48x48/apps
mkdir -p ~/.local/share/icons/hicolor/128x128/apps



# Copy icon to standard icon locations
if [ -f "$ASSETS_DIR/harmony_icon.png" ]; then
    cp "$ASSETS_DIR/harmony_icon.png" ~/.local/share/icons/hicolor/256x256/apps/harmony.png
fi

# Get the icon path for the desktop file
ICON_PATH="$HOME/.local/share/icons/hicolor/256x256/apps/harmony.png"

# Create desktop file
cat > ~/.local/share/applications/harmony.desktop << EOF
[Desktop Entry]
Name=Harmony Screen Share
Comment=P2P Screen Sharing Application
Exec=$BINARY_PATH
Icon=$ICON_PATH
Type=Application
Categories=Network;AudioVideo;RemoteAccess;
Keywords=screen;share;remote;desktop;streaming;
Terminal=false
StartupNotify=true
StartupWMClass=harmony
X-GNOME-SingleWindow=true

# KDE specific
X-KDE-SubstituteUID=false
X-KDE-Username=

# Actions for quick launch
Actions=Host;Viewer;

[Desktop Action Host]
Name=Start as Host
Exec=$BINARY_PATH host
Icon=$ICON_PATH

[Desktop Action Viewer]
Name=Start as Viewer
Exec=$BINARY_PATH viewer
Icon=$ICON_PATH
EOF



# Update desktop database
if command -v update-desktop-database &> /dev/null; then
    update-desktop-database ~/.local/share/applications
fi

# Update icon cache if available
if command -v gtk-update-icon-cache &> /dev/null; then
    gtk-update-icon-cache -f -t ~/.local/share/icons/hicolor 2>/dev/null || true
elif command -v update-icon-caches &> /dev/null; then
    update-icon-caches ~/.local/share/icons/hicolor 2>/dev/null || true
fi


