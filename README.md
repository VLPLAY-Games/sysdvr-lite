# sysdvr-lite

A tiny, dependency-light PC client for [SysDVR](https://github.com/exelix11/SysDVR) written in C, built for single-board computers that cannot afford the official .NET client.

It receives the video and audio stream of a Nintendo Switch over **USB** (or the TCP bridge) and shows it on an HDMI display without X11 or Wayland. On the target board (Orange Pi Zero 2W, Allwinner H618) it decodes 720p30 with the **hardware H.264 decoder** and scans the picture out through the **GPU**, using about 5-8% of one CPU core.

> The Switch side (the SysDVR sysmodule) is **not modified**. Install the stock SysDVR release on the console and use this program instead of the official client.

## Why

The official client is a .NET application with an ImGui/SDL front end. On an Orange Pi Zero 2W it only managed a slideshow at 720p30, even with the software decoder, because of runtime and conversion overhead. This client keeps the whole pipeline in C with pluggable backends, so you can pick the decoder that works best on your hardware.

## Features

- SysDVR protocol 02 and 03: USB (libusb) and TCP bridge transports, hot-plug with automatic reconnect, resync after corrupted TCP headers.
- Pluggable video decoders:
  - `v4l2`: native stateless V4L2 H.264 decoding for the Allwinner **Cedrus** VPU (H616/H618, mainline kernel). No ffmpeg involved. Includes its own SPS/PPS/slice-header parser.
  - `ffmpeg`: software decoding via libavcodec (or any libavcodec decoder by name), low-delay settings.
  - `null`: discards data, for network and protocol testing.
- Output without a window system: KMS/DRM with the display mode matched to the video, never scaled. Frames are imported as dmabuf into EGL/GLES and converted to RGB on the GPU straight into the scanout buffer. Falls back to a CPU conversion when no GPU stack is available.
- Audio over ALSA (48 kHz stereo S16).
- Low-latency queues: when the decoder falls behind, old packets are dropped and decoding resumes at the next keyframe.
- Runs on arm64 and x86-64 Linux.

## Status

Tested on an Orange Pi Zero 2W with Armbian (Debian 13, kernel 6.18) and a real Switch (SysDVR 6.3, protocol 03) over USB: 720p30, no dropped frames, hot-plug works, hardware output is bit-exact with libavcodec on the test streams.

Not done or not verified:

- The x86-64 build is only compiled, the hardware path is specific to Allwinner Cedrus. Use `--decoder ffmpeg` elsewhere.
- No audio/video synchronization (audio is played as it arrives).
- Hardware decoder limits: progressive video only, no B-frames, no MMCO/long-term references, no scaling matrices, no FMO. An error is logged when one of these is hit. The Switch encoder does not use them in the games tested so far.
- 720p only; the display output assumes the video size from the sysmodule.

## Build

Requirements: a C11 compiler, CMake, pkg-config. All libraries are optional and enable the matching feature when found.

| Feature | Debian/Ubuntu packages |
|---|---|
| USB transport | `libusb-1.0-0-dev` |
| Software decoder | `libavcodec-dev libavutil-dev` |
| Audio | `libasound2-dev` |
| DRM/KMS output | `libdrm-dev` |
| GPU conversion | `libgbm-dev libegl-dev libgles-dev libgl1-mesa-dri libegl-mesa0` |
| V4L2 decoder | kernel headers (`linux-libc-dev`) |

```bash
sudo apt install build-essential cmake ninja-build pkg-config v4l-utils \
    libusb-1.0-0-dev libavcodec-dev libavutil-dev libasound2-dev \
    libdrm-dev libgbm-dev libegl-dev libgles-dev libgl1-mesa-dri libegl-mesa0
cmake -S . -B build -G Ninja
cmake --build build
```

The configure step prints which features were enabled.

## Usage

```bash
# Switch over USB, hardware decoder, HDMI output
./build/sysdvr-lite --usb --decoder v4l2 --drm

# Switch over the network (TCP bridge mode in the sysmodule)
./build/sysdvr-lite --tcp 192.168.1.20 --decoder ffmpeg --threads 3 --drm
```

| Option | Description |
|---|---|
| `--usb` / `--tcp HOST` | Transport. Exactly one is required. |
| `--decoder null\|ffmpeg\|v4l2` | Video backend, default `ffmpeg`. |
| `--threads N` | Threads of the software decoder. |
| `--ffmpeg-name NAME` | Use a specific libavcodec decoder. |
| `--video-device /dev/videoN` | Device node for the `v4l2` backend, default `/dev/video0`. |
| `--drm[=/dev/dri/cardN]` | Show on the display via KMS. |
| `--out FILE` | Write raw decoded frames to a file instead of displaying them. |
| `--no-audio` / `--no-video` | Disable a stream. |
| `--audio-device NAME` | ALSA device, e.g. `plughw:CARD=HDMI,DEV=0`. |
| `--screen-off` | Turn the console screen off while streaming (protocol 03). |

Environment variables for debugging: `SYSDVR_NOGL=1` disables the GPU output path, `SYSDVR_DUMP=file` saves the scanout buffer as raw XRGB, `SYSDVR_PROF=1` prints timing of the CPU conversion path.

On the Switch, set SysDVR to USB mode (or TCP bridge mode for `--tcp`). When no console is connected, the client simply waits and starts streaming as soon as one appears.

### Autostart (systemd)

```bash
sudo cmake --install build --prefix /usr/local
sudo cp tools/sysdvr-lite.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sysdvr-lite
```

Edit the `ExecStart` line to change decoder or audio device. Logs: `journalctl -u sysdvr-lite -f`.

## Orange Pi Zero 2W notes

- Tested with Armbian (current, kernel 6.18). The mainline `cedrus` driver exposes `/dev/video0` + `/dev/media0`, and the display engine has **no YUV planes**, which is why the GPU does the colour conversion.
- Increase CMA if you see allocation errors: add `extraargs=cma=128M` to `/boot/armbianEnv.txt`.
- The Switch must be plugged into the USB port that works in host mode. Check with `lsusb` for `18d1:4ee0`.
- HDMI audio device is usually `plughw:CARD=HDMI,DEV=0`, check with `aplay -L`.
- `tools/probe_board.sh` prints everything relevant about a board (kernel, V4L2 nodes, DRM state).

## Testing without a console

```bash
./build/gen-h264 test.h264 1          # 720p30 test stream, 1 slice per frame (4 = multi-slice)
./build/mock-switch test.h264         # fake sysmodule on ports 9911/9922
./build/sysdvr-lite --tcp 127.0.0.1 --decoder v4l2 --no-audio --out hw.nv12
./build/ref-decode test.h264 ref.yuv  # offline libavcodec reference
python3 tools/compare_yuv.py ref.yuv hw.nv12   # checks the hardware output is bit-exact
```

## Layout

```
src/proto.*          wire format, handshake request building
src/transport_*.c    USB and TCP transports
src/queue.*          drop-oldest packet queue
src/decoder_*.c      null / ffmpeg / native V4L2 (Cedrus) decoders
src/h264_parse.*     SPS, PPS and slice header parser for the V4L2 decoder
src/sink_*.c         null, file and DRM/KMS (+EGL) outputs
src/audio_alsa.c     ALSA playback
tools/               mock console, stream generator, reference decoder, board probe, service file
docs/DESIGN.md       architecture and hardware notes
```

## Credits and license

The protocol was implemented from the [SysDVR](https://github.com/exelix11/SysDVR) documentation and by observing its behaviour. SysDVR is by exelix11 and is licensed under the GPL-2.0, which is also the license of this repository (see `LICENSE`). This project is not affiliated with Nintendo or with the SysDVR project.
