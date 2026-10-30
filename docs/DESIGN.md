# Design notes

## Protocol (checked against the SysDVR source, not only protocol.md)

- Packet header, 18 bytes little endian: magic `CCCCCCCC`, size u32, timestamp u64 (microseconds), flags u8, replay slot u8. Flags: bit0 video, bit1 audio, bit2 data, bit3 replay, bit4 multi-NAL, bit5 error. Maximum payload `0x54000`.
- USB: VID `18D1` / PID `4EE0`, bulk endpoints, one multiplexed stream, each packet read as a single transfer. (The TCP bridge of the sysmodule is not supported by this client.)
- Handshake: the console sends `SysDVR|NN\0` (10 bytes), the client answers with a 16-byte request (`AAAAAAAA`, version, MetaFlags, VideoFlags, AudioBatching, FeatureFlags, 6 reserved), the console replies with a u32 result (6 = OK; 72 bytes for protocol 03).
- Video is Annex-B H.264, 1280x720@30, with SPS/PPS injected by the console. NAL replay is not enabled.

## Architecture

```
transport (usb | test file) -> reader thread -> pqueue (drop oldest) -> decode thread -> sink
                                          -> pqueue               -> audio thread (ALSA)
```

- `transport.h` hides the channel; `proto.c` is plain (de)serialization.
- `decoder.h` is the backend interface (`null`, `ffmpeg`, `v4l2`). A frame may carry a dmabuf fd for zero-copy output.
- `sink.h`: `null` (FPS), `file` (raw frames), `drm` (KMS + EGL).
- The video queue is small (8 packets). When it overflows, the oldest packets are dropped and the decoder skips everything until the next IDR picture, because a dropped P-frame corrupts the rest of the GOP.

## Native Cedrus decoder (`decoder_v4l2.c`, `h264_parse.c`)

Facts established from the 6.18 kernel sources and by experiment:

- Cedrus is slice based with `START_CODE_NONE`: every slice NAL goes to its own OUTPUT buffer, **with** the NAL header byte and **without** `00 00 01`. Multi-slice frames use `V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF` on all but the last slice.
- Per slice a media request carries SPS, PPS, scaling matrix, decode params, slice params and (when needed) prediction weights.
- `header_bit_size` = 8 (NAL header) + slice header bits in the RBSP (emulation prevention bytes excluded).
- DPB entries are matched by `reference_ts`, the OUTPUT buffer timestamp in nanoseconds; every frame gets a unique one.
- CAPTURE offers NV12, tiled and AFBC formats. NV12 is used; the buffers are exported as dmabuf for the GPU.
- Reference handling is a sliding window with P-frame list reordering; B-frames, MMCO and field pictures are rejected with an explicit error.
- Verified bit-exact against libavcodec on CABAC streams with weighted prediction, with 1 and 4 slices per frame.

## Display path (`sink_drm.c`)

The mainline display engine of the H618 has only RGB planes. The decoder's NV12 dmabuf is imported as an `EGL_LINUX_DMA_BUF_EXT` image, sampled through `GL_OES_EGL_image_external` (BT.709, narrow range) and rendered into one of two dumb buffers that are shared with the GPU through dmabuf and flipped with the page-flip ioctl. The EGL context is created lazily in the drawing thread because contexts are per-thread. If the GPU path cannot be initialised, frames are converted on the CPU (reading uncached decoder memory is slow, so the frame is copied once first).

## Ideas

- End-to-end latency measurement.
- Support for more decoder features (B-frames, MMCO) if some game needs them.

## Audio/video synchronization

Packets carry timestamps (microseconds) taken from the same console clock for both streams. Video is presented as soon as it is decoded, never delayed. The audio thread computes the console timestamp of the sample that is audible right now (`packet.ts - ALSA delay`) and compares it with the timestamp of the frame last handed to the display. A skew within [-120 ms, +40 ms] is left alone, beyond that audio packets are skipped (audio late) or silence is inserted (audio early). Audio is naturally a little late because the console sends it in batches; measured on a real console the best setting is `--audio-batching 1` (average skew about -20 ms, no corrections needed), 0 and 3 were worse (-80 ms and -108 ms).

## Console stream properties

Measured with `SYSDVR_H264_INFO=1`: High profile, level 3.2, 1 reference frame, POC type 2, CABAC with 8x8 transform, no weighted prediction, one slice per picture, IDR every 30 pictures, no B-frames, no MMCO, no reordering commands. The hardware decoder therefore needs none of the features it rejects.
