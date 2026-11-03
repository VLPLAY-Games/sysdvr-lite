# Switch module: SysDVR with the console screen watchdog

[English](README.md) | [Русский](README.ru.md)

This is the SysDVR sysmodule (USB only, upstream version 6.3, protocol 03) with one added feature: **the console screen
comes back by itself when the game leaves the screen** (HOME menu, a controller prompt, a system window), and goes dark
again when the video is back.

Why it is needed: with "turn off the screen while streaming" SysDVR switches the backlight off at connect and switches
it on only when the client disconnects. In the HOME menu the video thread of the sysmodule waits inside the grc service
for the next frame and sends nothing, so it never notices anything and the screen stays off until the game returns. You
had to press the power button twice to see what the console wants.

## What the change is
- a watchdog thread looks twice a second: if the client asked for the screen to be turned off and no video packet went
  out for 2.5 seconds (6 seconds before the first picture of a connection, which can take about 3 seconds to start: so the screen does not blink on at the first connection), the backlight is switched on (the same call SysDVR already makes when the client disconnects);
  when video goes out again it is switched off again
- all backlight calls are serialized with a mutex (the video thread and the new thread both make them)
- 61 lines added, none removed: [screen-watchdog.patch](screen-watchdog.patch). Without a client that asked for "screen
  off" it does nothing, and the protocol is unchanged, so the stock client and `sysdvr-lite` both work with it.
- **USB only**: TCP, RTSP and the config app are cut out (the official USB-only build does the same), so it cannot be
  switched to another mode. Building the sources with the patch removed gives the official USB-only build.

## Installing
Each release has two files built by the `Switch module` workflow:

- **`exefs.nsp`**, if SysDVR is already installed on the console: back up
  `SD:/atmosphere/contents/00FF0000A53BB665/exefs.nsp`, replace it with this file (keep `flags/boot2.flag` and
  `toolbox.json` as they are) and reboot the console completely.
- **`SysDVR-USB-screenwatch.zip`**, if the console has no SysDVR: unzip it to the **root of the SD card** (it creates
  `atmosphere/contents/00FF0000A53BB665/` with `exefs.nsp`, `toolbox.json` and `flags/boot2.flag`) and reboot the console.

Then start `sysdvr-lite` (the console must be in USB mode, which is the only mode of this build). To check: HOME for a
few seconds, the console screen lights up by itself; back to the game, it goes dark and the picture returns.

Rolling back: put the original `exefs.nsp` back. If the console shows an error screen at start, note its code and
remove the folder `SD:/atmosphere/contents/00FF0000A53BB665/` (or only its `flags/boot2.flag`) on a PC.

I could build and review this module but I only tested it on one Switch Lite (firmware and Atmosphère as of November
2026): it worked in the HOME menu and in the controller prompt of Mario Party.

## Building
`./build.sh` needs devkitA64 and libnx (devkitPro) and python3; it writes `dist/exefs.nsp` and
`dist/SysDVR-USB-screenwatch.zip`. The workflow in `.github/workflows/switch-module.yml` does the same in the
`devkitpro/devkita64` container and attaches both files to a release when it is published. (The build ID inside the file depends
on the path it was built in, so two builds of the same sources can differ by a few bytes.)

## License
GPL-2.0, like SysDVR and this repository. The sysmodule is [SysDVR](https://github.com/exelix11/SysDVR) by exelix11,
upstream commit `804fd36`; only the files listed in the patch were changed. It is not affiliated with the SysDVR project.
