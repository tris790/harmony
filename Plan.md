Implementation Plan - Handmade Screen Share
A rigorous C application for screen sharing on Linux (Wayland), suitable for P2P streaming with minimal latency.

User Review Required
IMPORTANT

Dependencies: We will link libavcodec, libavformat, libavutil, and libswscale. This requires these development libraries to be present on the system. Platform: We will write the Wayland platform layer from scratch (connecting to the registry, setting up EGL) to verify "First Principle" skills. SDL or GLFW would be easier but less "Handmade". Web Viewer: This will receive the same H.264 stream packetized for WebSocket.

Coding Standards (Handmade Hero Style)
IMPORTANT

Performance First: We prioritize data-oriented design.

Memory: Use Arena Allocators for everything. No malloc/free spaghetti. We will pass a MemoryArena context to functions.
Structs: Prefer "fat structs" (PODs) over excessive pointer chasing. Data should be contiguous.
Control Flow: Direct code execution. Avoid callbacks and complex obsession-oriented programming (OOP).
Simplicity: No hidden control flow, no exceptions, minimal dependencies.
Proposed Changes
Project Structure
src/
main.c: Entry point.
platform/:
linux_wayland.c: Raw Wayland client code, EGL setup, Input handling.
codec/:
av_encoder.c: Wraps libavcodec for host (RGB -> YUV -> H.264).
av_decoder.c: Wraps libavcodec for viewer (H.264 -> YUV).
net/:
udp_transport.h/c: Custom UDP protocol.
p2p_handshake.c: Logic to exchange IPs manually.
ui/:
simple_ui.h/c: Custom Immediate Mode GUI (Draw rects/text for Settings).
tests/:
test_codec.c: Feeds procedural texture -> Encoder -> Decoder -> Compare.
test_net.c: Mocks sockets to test fragmentation/reassembly.
1. Architecture & Abstractions
os_api: Windowing, Input, Threading.
codec_api: init_encoder, encode_frame, decode_frame.
ui_api: ui_begin, ui_button, ui_end.
2. Networking (UDP Custom Protocol)
Packet Structure:
| HEADER (Seq, FrameID, FragID) | DATA |
Flow:
Host captures frame -> Encodes (libavcodec) -> Fragments -> UDP Send.
Client Receiving -> Reassembles Frame -> Decodes (libavcodec) -> Uploads to Texture -> Renders.
3. Verification Plan (Automated)
We will create a tests binary.

Video Test: generate 60 frames of a moving square. Encode it. Decode it. Assert that the decoded square is roughly in the same position (PSNR check).
Network Test: Serialize a large frame (100KB). Pass it through a "lossy channel" function (drops 1 packet). Verify the receiver handles it (either drops frame or requests NACK - strict low latency usually just drops).
4. UI & Settings
Overlay for entering the "Target IP" and "Bitrate".
"Handmade" style: We render this directly to the OpenGL context using a font atlas.
5. Web Viewer
A small HTTP/WS server (written in C, no extensive libs, maybe http-parser implementation or raw socket handling).
Sends the raw H.264 NAL units over WebSocket.
Client side: use jmuxer.js (very minimal) to play the raw H.264 stream.
Verification Plan
Automated Tests
Run ./build.sh test to execute:

test_codec: Validates FFmpeg linking and parameter setup.
test_net: Validates fragmentation logic.
Manual Verification
Visual Latency: Run host/client on same screen. Move mouse. Observe delay.
Wayland Compatibility: Ensure it runs on standard Compositors (GNOME/Mutter, Sway/Wlroots).