# Changelog

## 1.2.1

### Fixes
- Parser: weighted prediction tables with a weight denominator above 7 (invalid in a stream) are rejected instead of shifting by a huge amount (undefined behaviour). Found by the new parser tests run under UBSan.

- Watchdog: the client tells systemd that it is alive and asks for a 60 second watchdog itself, so a hang (the reader loop stops turning round, or one picture takes over ten seconds to decode) restarts it, not only a crash. An old service file or an old client is not affected.
- Memory: every allocation is checked; a failed one drops the packet or ends the start with a message instead of crashing.
- `tools/fetch-binary.sh` verifies the archive against the `.sha256` file that the release workflow now attaches (an older release without it only prints a warning).

### Features
- After the boot (and when the console is unplugged) the text console shows in English and Russian that everything is running and the Switch can be connected. It sets a Cyrillic console font when `setfont` and the fonts are there (the font grows with the screen), otherwise only the English text is written. `--no-banner` turns it off.
- `tools/readonly-root.sh enable|disable|status`: optional read-only root filesystem (overlayroot) that keeps the SD card from being written to at all (not tested on a real board yet); `install.sh` and `update.sh` refuse to run while it is on.

### Switch module
- The console screen blinked on at the first connection after the board started. The first picture of a connection takes 3-4 seconds, SysDVR drops a client that gets nothing for 1.5 seconds, and when it drops one it gives the screen back; the client then connected again and the screen went dark again. Until the first video packet of a connection the limit (and the screen watchdog's) is now 8 seconds. The client also says in the log when the console drops a session (`the console says hello again`).

### Tests
- CI: parser tests under ASan and UBSan (hand-made cases and random damage to the NAL units of a generated stream), a damaged-stream replay, and a weekly fuzz workflow.

## 1.2.0

### Fixes
- Decoder: after a failed picture all buffers are taken back and the streams restarted. A capture buffer left queued used to fail every next picture until the client gave up and systemd restarted it.
- Decoder and parser: an SPS or PPS with an id of 32 or more no longer reads past the SPS table; an invalid Exp-Golomb code (32 zero bits) and more than 16 reference frames are rejected.
- USB: a read that timed out after part of a packet came is treated as a broken packet, not as "nothing received"; the endpoints are forgotten before they are looked up again.

### Documentation
- README: measurements (latency of the board, CPU load, temperature) and a comparison with the other ways to stream a Switch Lite, a tip about a frame rate limiter, and a note on the rare 4K TV that shows nothing before the stream.

### Installation
- `install.sh --video-mode WxH[@HZ]` makes the kernel use that display mode from boot (`off` removes it again), for a TV that sometimes shows nothing before the stream.

### Known issues
- At the first connection the console screen may blink on for a moment and only then turn off. It does not affect the stream. Not fixed yet.

## 1.1.0

### Client
- The picture comes back by itself after the TV is switched off and on or the HDMI cable is replugged; a different display after a replug restarts the client (systemd brings it back). When no display is connected at start the client waits for one instead of exiting every two seconds.
- The sound output is opened again when it was not available at start (the HDMI card was not ready) or stops answering. alsa-lib error spam is gone from the log.
- `--scale fit|none`: when the display has no mode of the video's size, the smallest mode that contains the video is chosen and the picture is fitted to it (it used to be a small centered picture).
- The console is found at once when plugged in (libusb hot-plug event instead of polling once a second), and the log says `SysDVR console connected`.

- When the console sends no video for three seconds (HOME menu, a controller prompt, a system window) the client draws a message on the display (Russian with an English copy) instead of leaving a frozen picture; `--no-notice` turns it off, `install.sh --stock-sysdvr` installs without it for a console with the stock SysDVR. `SYSDVR_PKTSTAT=1` prints packet and frame statistics once a second.
- The client ends the stream (USB reset) when the display is gone for two seconds and waits for a display before streaming, so the console screen is not left off with nobody watching.

### Switch module
- `switch-module/`: the SysDVR sysmodule (USB only, upstream 6.3) with one added feature: the console screen comes back by itself after 2.5 seconds without video (HOME menu, a controller prompt) and goes dark again when the video is back, instead of staying off until the game returns. `switch-module/build.sh` and the `Switch module` workflow build `exefs.nsp` (to replace the file of an installed SysDVR) and `SysDVR-USB-screenwatch.zip` (everything for a console that has no SysDVR yet, unzip to the SD card); the workflow attaches both to a release when it is published.

### Installation and service
- Service options live in `/etc/default/sysdvr-lite`; `update.sh` moves the options of an old unit there. The service unit is hardened (`systemd-analyze security`: 9.4 UNSAFE to 5.6 MEDIUM) and `update.sh` keeps the unit in step with the repository.
- `install.sh --binary` and `update.sh` install a prebuilt Debian 13 arm64 binary from the latest release (built by a release workflow when a release is published), without a compiler or development packages. `--force` lets other arm64 Debian based systems try the installer.

### Known issues
- At the first connection the console screen may blink on for a moment and only then turn off. It does not affect the stream. Not fixed yet, planned for 1.2.0.

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
