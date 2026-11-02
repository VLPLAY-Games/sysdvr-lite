# Changelog

## 1.0.0

First stable release: a small C client for [SysDVR](https://github.com/exelix11/SysDVR) that shows a Nintendo Switch on an HDMI display from an Orange Pi Zero 2W, with no PC, X11 or Wayland.

### Client
- SysDVR protocol 02 and 03 over USB, with hot-plug and automatic reconnect.
- Hardware H.264 decoding on the Allwinner H618 (Cedrus, stateless V4L2) with its own SPS/PPS/slice parser. The output is bit-exact with libavcodec on the test streams. A software ffmpeg decoder is the fallback.
- KMS/DRM output without a window system. The GPU (Mali-G31 through EGL/GLES) converts the decoded frames straight into the scanout buffer.
- Audio over ALSA (HDMI) with A/V synchronisation by the console timestamps.
- The console screen is turned off while streaming (`--keep-screen` to disable). When the console is unplugged the display returns to the terminal, and the picture comes back when it is plugged in again.
- Measured on the board: about 14 ms average software latency (25 ms worst) at 720p30, 5-8% of one CPU core.
- `--version` prints the version and the features of the build.

### Installation
- One-command installer, also from `curl ... | sudo bash`: syncs the clock if needed, installs git and the packages from `packages.txt`, clones the repository, sets the CMA pool to 128 MB, builds the client and enables a systemd service.
- `update.sh` pulls the latest code, installs build packages that were removed, rebuilds and restarts the service.
- Supported: Orange Pi Zero 2W with Armbian (Debian based), arm64 only.

### Tools and project
- `tools/doctor.sh` collects a report for bug reports (board, system, USB, decoder, display, audio, service log) without the host name or addresses.
- CI: shellcheck for the scripts, and an arm64 build with a decoder replay test against the reference decoder.
- Issue templates for bug reports and compatibility reports. README in English and Russian.
- `hardware/`: a 3D-printable dock for the Switch Lite with a block for the board (an early version, see `hardware/README.md`). The stand parts are a remix of a model by MM_Printing under CC BY-NC 4.0, the rest is under the repository license.

### Tested on
Orange Pi Zero 2W, Armbian (Debian 13, kernel 6.18), Switch Lite with SysDVR 6.3 (protocol 03) over USB, two Sony TVs and an MSI monitor. Fresh installs and updates were run on clean cards.
