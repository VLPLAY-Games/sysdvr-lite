# sysdvr-lite

<p align="center"><b>English</b> &nbsp;|&nbsp; <a href="README.ru.md">Русский</a></p>

A tiny, dependency-light client for [SysDVR](https://github.com/exelix11/SysDVR) written in C, built for the Orange Pi Zero 2W (Allwinner H618, arm64 Linux) which cannot afford the official .NET client.

It receives the video and audio stream of a Nintendo Switch over **USB** and shows it on an HDMI display without X11 or Wayland. On the target board (Orange Pi Zero 2W, Allwinner H618) it decodes 720p30 with the **hardware H.264 decoder** and scans the picture out through the **GPU**, using about 5-8% of one CPU core.

> The Switch side (the SysDVR sysmodule) is **not modified**. Install the stock SysDVR release on the console and use this program instead of the official client.

## Why

The official client is a .NET application with an ImGui/SDL front end. On an Orange Pi Zero 2W it only managed a slideshow at 720p30, even with the software decoder, because of runtime and conversion overhead. This client keeps the whole pipeline in C and uses the board's hardware decoder.

## Features

- SysDVR protocol 02 and 03 over USB (libusb), with hot-plug and automatic reconnect.
- Pluggable video decoders:
  - `v4l2`: native stateless V4L2 H.264 decoding for the Allwinner **Cedrus** VPU (H616/H618, mainline kernel). No ffmpeg involved. Includes its own SPS/PPS/slice-header parser.
  - `ffmpeg`: software fallback via libavcodec (or any libavcodec decoder by name), low-delay settings.
  - `null`: discards data, for network and protocol testing.
- Output without a window system: KMS/DRM with the display mode matched to the video, never scaled. Frames are imported as dmabuf into EGL/GLES and converted to RGB on the GPU straight into the scanout buffer. Falls back to a CPU conversion when no GPU stack is available.
- Audio over ALSA (48 kHz stereo S16) with A/V synchronization: both streams carry console timestamps, the audio follows the picture. Video is never delayed.
- The console screen is turned off while the client is connected and turned back on when it disconnects (the SysDVR sysmodule does this on request of the client, protocol 03). Disable with `--keep-screen`.
- When the console is unplugged, the display goes back to the terminal (no frozen last frame) and the picture returns when it is plugged in again.
- Low-latency queues: when the decoder falls behind, old packets are dropped and decoding resumes at the next keyframe.
- arm64 Linux only (the build refuses to configure on anything else).

## Status

Tested on an Orange Pi Zero 2W with Armbian (Debian 13, kernel 6.18) and a real Switch (SysDVR 6.3, protocol 03) over USB: 720p30, no dropped frames, hot-plug works, hardware output is bit-exact with libavcodec on the test streams. The full path (fresh Armbian image, `install.sh`, reboot, autostart, picture and sound) was verified on a clean system, and `update.sh` on an installed one.

Measured on that setup: the software part of the pipeline (packet received over USB to frame on screen, measured at vblank) takes about 14 ms on average, 25 ms at worst. The console's own capture and encoding delay is not included. Audio plays on average about 20 ms behind the picture. No published latency figures for the official SysDVR clients exist to compare against.

Decoder limits: progressive video only, no B-frames, no MMCO/long-term references, no scaling matrices, no FMO. This is not a practical limit: the console encoder always emits the same fixed stream (High profile, level 3.2, one reference frame, POC type 2, CABAC, one slice per frame, IDR every 30 frames), which is why the SysDVR sysmodule and clients hardcode its SPS/PPS. `SYSDVR_H264_INFO=1` prints the stream properties that were seen. An explicit error is logged if anything unsupported shows up.

Not verified: other Allwinner boards (H616 variants should work but are untested), 1080p modes other than 1280x720 output (the picture is centered, never scaled).

## Requirements

- An **Orange Pi Zero 2W** running **Armbian** (Debian based, the tested image is Debian 13 "trixie" minimal/server with the `current` kernel). An 8 GB card is enough, the installed system uses about 2-3 GB.
- A Nintendo Switch with the stock [SysDVR](https://github.com/exelix11/SysDVR) sysmodule set to **USB mode**, and a USB cable to the board's host-capable USB port.
- An HDMI display (a TV or monitor with a 1280x720 mode).
- Internet access on the board for the installation. The system clock does not need to be set up by hand: the board has no battery-backed clock, and `install.sh` turns on network time itself if the clock is not synchronized (otherwise `apt` fails with "Release file is not valid yet").

## First boot of a fresh Armbian image

Skip this if your board already runs Armbian and is online.

1. Boot the board from the freshly written card and log in as `root` (on the console or over SSH). The first login asks you to set a password for `root`.
2. If it then offers to create a new user, you can press `Ctrl+C` to skip it and keep working as `root`.
3. Connect the board to the network: run `armbian-config`, then choose **Network**, then **BNS001 (Basic network setup)**. Pick **Wi-Fi** (the Orange Pi Zero 2W has no Ethernet port, so Wi-Fi is the usual case) or a wired connection, select your network, enter its password and confirm with **OK** wherever it asks. The menu names can differ a little between Armbian versions.
4. Check that you are online, for example `ping -c 3 github.com`, then continue with the install below.

## Quick install (Orange Pi Zero 2W only)

> **The install script supports only the Orange Pi Zero 2W running Armbian (Debian based).** It refuses to run on any other board or distribution. The client itself only builds for arm64 Linux.

One command, nothing to download first (it asks for confirmation, add `-s -- --yes` after `bash` to skip the question):

```bash
curl -fsSL https://raw.githubusercontent.com/VLPLAY-Games/sysdvr-lite/main/install.sh | sudo bash
```

The script clones the repository to `/opt/sysdvr-lite` (change with `--dir`). You can also clone it yourself and run it from the checkout:

```bash
git clone https://github.com/VLPLAY-Games/sysdvr-lite
cd sysdvr-lite
sudo ./install.sh
```

The board needs internet access during the install. The script:

1. checks that it runs on an Orange Pi Zero 2W with Armbian/Debian,
2. turns on network time if the clock is not synchronized, and waits for it,
3. installs git if it is missing and clones the repository (skipped when run from a checkout),
4. installs all build and runtime packages from `packages.txt` with `apt`,
5. sets the CMA pool to 128 MB in `/boot/armbianEnv.txt` (a backup is saved next to it, skip with `--no-cma`),
6. builds the client and installs it to `/usr/local/bin`,
7. creates and enables the `sysdvr-lite` systemd service, so it starts at boot and waits for the Switch.

If the CMA size changed, the script tells you to reboot (`sudo reboot`). After that the service starts by itself at every boot: with SysDVR in USB mode on the console, plug it in and the picture appears on the HDMI display, no keyboard needed. Unplugging the console returns the display to the terminal, plugging it in again brings the picture back.

Verify the installation:

```bash
systemctl is-active sysdvr-lite                 # active
grep Cma /proc/meminfo                          # CmaTotal: 131072 kB
journalctl -u sysdvr-lite -n 20 --no-pager      # cedrus / Mali-G31 / GPU conversion enabled
```

Options: `--audio-device NAME` (default `plughw:CARD=HDMI,DEV=0`, see `aplay -L`), `--decoder v4l2|ffmpeg` (default `v4l2`), `--dir PATH` (where to clone when run through curl, default `/opt/sysdvr-lite`), `--no-cma`, `--yes` (no confirmation prompt, required when there is no terminal). Through curl the options go after `bash -s --`, for example `... | sudo bash -s -- --yes --decoder ffmpeg`.

## Updating

On the board, inside the repository (`/opt/sysdvr-lite` if you installed through curl):

```bash
sudo /opt/sysdvr-lite/update.sh
```

It pulls the latest code, checks that every package listed in `packages.txt` is still installed (and installs the ones that are missing, for example if you removed them or the new version needs more), rebuilds, installs the new binary and restarts the service. The CMA setting and your service options (decoder, audio device) are left untouched. Use `./update.sh --no-pull` to rebuild a local checkout without fetching.

## Changing settings

The service runs `sysdvr-lite` with the options from its unit file. To change the audio device or decoder, either re-run the installer with the options (it keeps your CMA setting and rewrites the unit, it runs `apt` again so it needs internet access):

```bash
sudo ./install.sh --yes --audio-device plughw:CARD=Codec,DEV=0 --decoder v4l2
```

or edit `ExecStart` in `/etc/systemd/system/sysdvr-lite.service` and apply it:

```bash
sudo systemctl daemon-reload && sudo systemctl restart sysdvr-lite
```

`update.sh` never touches these options. All command line options are listed in [Usage](#usage).

## Uninstalling

```bash
sudo systemctl disable --now sysdvr-lite
sudo rm /etc/systemd/system/sysdvr-lite.service /usr/local/bin/sysdvr-lite
sudo systemctl daemon-reload
```

To undo the CMA change, restore the backup that the installer made: `sudo cp /boot/armbianEnv.txt.sysdvr-lite.bak /boot/armbianEnv.txt` and reboot. Installed packages are left in place.

## Troubleshooting

| Symptom | What to check |
|---|---|
| `apt` says "Release file is not valid yet" during install | The clock is wrong. `install.sh` fixes this itself; if you see it from `update.sh`, run `sudo timedatectl set-ntp true`, wait until `timedatectl` shows `System clock synchronized: yes`. |
| The installer refuses to run | It only supports the Orange Pi Zero 2W with Armbian/Debian. See the message for what was detected. |
| No picture, nothing in the log about the console | `lsusb` must show `18d1:4ee0` (SysDVR). If not: SysDVR is not in USB mode, wrong cable (it must carry data), or the wrong USB port. |
| No picture, `journalctl -u sysdvr-lite` shows errors | Missing `/dev/video0` means the Cedrus driver is not loaded (try `--decoder ffmpeg`). "decoder keeps failing" makes the service restart itself, send the log lines before it. |
| Picture works but no sound | Pick the ALSA device with `aplay -L` and set it as shown in [Changing settings](#changing-settings). HDMI is `plughw:CARD=HDMI,DEV=0`. |
| Audio and picture out of sync | Try `--av-offset` (milliseconds, positive = audio later) and `SYSDVR_SYNC=1` to see the measured skew. |
| The console screen stays off after disconnecting | The sysmodule restores it itself a moment after the connection ends. If it does not, lock and unlock the console (power button), or start the client with `--keep-screen` to never turn it off. SysDVR itself marks this feature as experimental. |
| Black screen with the console unplugged | That is the terminal; log in on the display or over SSH. The picture returns on its own when the console is plugged in. |
| High CPU | The log should say `GPU conversion enabled`. If it falls back to CPU conversion, install the GPU packages (see Manual build). |

## Manual build

Requirements: a C11 compiler, CMake, pkg-config. All libraries are optional and enable the matching feature when found.

| Feature | Debian/Ubuntu packages |
|---|---|
| USB transport (required) | `libusb-1.0-0-dev` |
| Software decoder fallback | `libavcodec-dev libavutil-dev` |
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
# Switch over USB, hardware decoder, HDMI output (this is what the service runs)
./build/sysdvr-lite --decoder v4l2 --drm --audio-device plughw:CARD=HDMI,DEV=0
```

The client waits until a console running SysDVR in **USB mode** shows up, and reconnects after it is unplugged.

| Option | Description |
|---|---|
| `--decoder null\|ffmpeg\|v4l2` | Video backend, default `ffmpeg`. Use `v4l2` for the hardware decoder. |
| `--threads N` | Threads of the software decoder. |
| `--ffmpeg-name NAME` | Use a specific libavcodec decoder. |
| `--video-device /dev/videoN` | Device node for the `v4l2` backend, default `/dev/video0`. |
| `--drm[=/dev/dri/cardN]` | Show on the display via KMS. |
| `--out FILE` | Write raw decoded frames to a file instead of displaying them. |
| `--no-audio` / `--no-video` | Disable a stream. |
| `--audio-device NAME` | ALSA device, e.g. `plughw:CARD=HDMI,DEV=0`. |
| `--audio-batching 0..3` | Audio packets the console collects before sending, default 1. |
| `--av-offset MS` | Shift audio relative to video (positive = audio later). |
| `--keep-screen` | Do not turn the console screen off while streaming (it is turned off by default). |
| `--test-file FILE.h264` | Replay an Annex-B file instead of using USB (decoder testing). |

Environment variables for debugging: `SYSDVR_NOGL=1` disables the GPU output path, `SYSDVR_DUMP=file` saves the 60th frame of the scanout buffer as raw XRGB (turn it into a PNG screenshot with `tools/xrgb2png.py`), `SYSDVR_LATENCY=1` prints pipeline latency, `SYSDVR_SYNC=1` prints the audio/video skew, `SYSDVR_H264_INFO=1` prints stream properties, `SYSDVR_PROF=1` prints timing of the CPU conversion path.

### Autostart (systemd)

`install.sh` does this for you. To do it by hand:

```bash
sudo cmake --install build --prefix /usr/local
sudo cp tools/sysdvr-lite.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now sysdvr-lite
```

Edit the `ExecStart` line to change decoder or audio device. Logs: `journalctl -u sysdvr-lite -f`.

## Orange Pi Zero 2W notes

- Tested with Armbian (current, kernel 6.18). The mainline `cedrus` driver exposes `/dev/video0` + `/dev/media0`, and the display engine has **no YUV planes**, which is why the GPU does the colour conversion.
- `install.sh` raises CMA to 128 MB by adding `extraargs=cma=128M` to `/boot/armbianEnv.txt` (the default 64 MB is enough for 720p but leaves little headroom).
- The Switch must be plugged into the USB port that works in host mode. Check with `lsusb` for `18d1:4ee0`.
- HDMI audio device is usually `plughw:CARD=HDMI,DEV=0`, check with `aplay -L`.
- `tools/probe_board.sh` prints everything relevant about a board (kernel, V4L2 nodes, DRM state).

## Testing without a console

The test file option replays a stream at 30 fps through the whole pipeline:

```bash
./build/gen-h264 test.h264 1                         # 720p30 test stream, 1 slice per frame (4 = multi-slice)
./build/ref-decode test.h264 ref.yuv                 # offline libavcodec reference
./build/sysdvr-lite --decoder v4l2 --test-file test.h264 --out /tmp/hw.nv12
python3 tools/compare_yuv.py ref.yuv /tmp/hw.nv12    # checks the hardware output is bit-exact
```

Stop the service first (`systemctl stop sysdvr-lite`), only one program can use the decoder at a time, and write the output to a RAM disk like `/tmp`: an SD card cannot keep up with 40 MB/s of raw frames.

## Layout

```
src/proto.*          wire format, handshake request building
src/transport_*.c    USB transport and the test-file replay
src/queue.*          drop-oldest packet queue
src/decoder_*.c      null / ffmpeg / native V4L2 (Cedrus) decoders
src/h264_parse.*     SPS, PPS and slice header parser for the V4L2 decoder
src/sink_*.c         null, file and DRM/KMS (+EGL) outputs
src/audio_alsa.c     ALSA playback
install.sh           one-shot installer for the Orange Pi Zero 2W (Armbian), also runs through curl
update.sh            pull, install missing packages, rebuild and restart the service
packages.txt         the apt packages both scripts use
tools/               stream generator, reference decoder, comparison script, board probe, service file
docs/DESIGN.md       architecture and hardware notes
README.ru.md         this file in Russian
```

## Credits and license

The protocol was implemented from the [SysDVR](https://github.com/exelix11/SysDVR) documentation and by observing its behaviour. SysDVR is by exelix11 and is licensed under the GPL-2.0, which is also the license of this repository (see `LICENSE`). This project is not affiliated with Nintendo or with the SysDVR project.

The 3D-printable dock in `hardware/` is licensed differently: its two stand parts are a remix of [Working Nintendo Switch Lite Charging Dock](https://www.printables.com/model/555711-working-nintendo-switch-lite-charging-dock) by [MM_Printing](https://www.printables.com/@MM_Printing) and stay under CC BY-NC 4.0 (non-commercial use, credit the author). Details in [hardware/README.md](hardware/README.md#license-and-credits).
