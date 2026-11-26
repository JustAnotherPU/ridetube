# ytdec

The video half of RideTube (BMW iDrive 7 MGU, Apollo Lake,
Intel HD 505). Reads what the car's fetcher (`jap-ridetube-d/ytfetch.py`) writes, decodes H.264
or VP9 (profile 0, through the unit's VP9 decoder plugin) on the GPU with the unit's Intel Media SDK, scales it with MSDK VPP and writes
I420 frames to mgu-render's video file `/dev/shm/mgu-video-ridetube` (MGVF,
mgu-render `PROTOCOL.md`, "Video"), paced by the audio clock that `ytd.js`
sends on stdin.

The decoder (`src/decoder_msdk.*`), the NAL reader (`src/nal_reader.*`,
`test/nal_reader_test.cpp`), the ABI tools (`tools/*.py`), the symbol lists
(`abi/*.symbols`) and `docker/Dockerfile` are copied from Robert Nisipeanu's
waze-on-cluster, GPL-3.0-or-later (`abi/libvsomeip3.so.3.symbols`: MPL-2.0),
their notices kept. Changes to the
decoder: display order (`DecodedOrder 0`) with per-access-unit timestamps,
VPP fit/fill into the output size, NV12 handed out instead of an atlas, and
MSDK GPU copy (`MFXInitEx`, `GPUCopy ON`) for the VPP output to system memory.

## Build

```sh
export TMPDIR=$HOME/tmp-docker     # macOS: Docker cannot mount /var/folders
make test      # host: NAL reader, MGYI index, MGVF writer, pacing/seek/tail/eof on a fake decoder
make target    # build/target/ytdec, in the image of docker/Dockerfile
               # (Ubuntu 18.04, gcc 7.5, glibc 2.27), against stubs of the unit's
               # libmfxhw64 / libva / libva-drm (abi/*.symbols), imports and glibc checked
```

Nothing is copied from the unit: the binary links the unit's
`/usr/lib/libmfxhw64.so`, `libva.so.1`, `libva-drm.so.1` at run time.

## Interface

```
ytdec --dir <folder> --rect <W>x<H> [--fill] [--no-upscale] [--log <file>]
```

- `<folder>`: `index.bin` (MGYI) and `video.h264` (Annex-B), or
  `video.h264.part` while it is still being fetched (the rename keeps the
  inode, so the open file follows). Both may grow; ytdec waits for them.
  The index's u32 at offset 20 is the codec: 0 (every index before VP9, and
  every H.264 one) = H.264 Annex-B; `"VP9 "` = the same file name holding raw
  VP9 frames, one index record per frame (a superframe is one record, passed
  whole), key = a keyframe = a seek and start point.
- `--rect`: MGVF frame size, default 1280x720; even, 16..2048, `W*H*3` at most
  12 MiB. Fit (letterbox/pillarbox, black bars) by display aspect, filling instead when that crops at most 15 %; `--fill`
  crops the centre instead.
- `--no-upscale`: the frame is the picture alone, no bars, at most at the
  source's resolution (rows): a 854x480 video fitted into 1920x720 is a
  854x480 frame, filled it is the 854x320 centre band; a 1080p one is scaled
  down to 1280x720. The picture keeps the aspect it had in the rect (nearest
  even width), so the renderer's op 19, which fits the frame into the rect the
  app gives it, puts it where the bars did (within 1 px on the full layout)
  when the app draws into the rect's aspect (RideTube 0.6.6, `vbox`).
  The MGVF file is opened at `--rect` and resized to the first frame, and again
  whenever the decoder's frame size changes (`src/fit.h`, `test_fit`).
- `--log`: log file (appended); stderr otherwise.

stdin, one command a line (EOF = quit):

| Command | Effect |
|---|---|
| `clock <elapsed_ms> <mono_ms>` | media position `elapsed_ms` was true at CLOCK_MONOTONIC `mono_ms`; pos = elapsed + (now − mono) |
| `pause` / `resume` | freeze pos / re-anchor it at now; after 60 s of pause the decoder session closes, and `resume` then seeks to the paused pos first (below) |
| `seek <ms>` | restart at the last keyframe ≤ ms whose access unit has an SPS, decode without VPP up to ms; shows the target frame at once (also while paused or before any clock); re-anchors the clock to ms until the next `clock` |
| `video 0` / `video 1` | stop decoding and writing and close the decoder session (the clock still runs) / seek to the current pos and go on |
| `quit` | exit 0 |

Before the first `clock` (and while paused) ytdec shows one frame (the first,
or the seek target) and holds. A frame is shown when pts ≤ pos + 8 ms, dropped
when more than 2 frame durations late; frames certain to be late are decoded
without VPP.

Idle: an open MSDK session costs ~4 % of a core with nothing to decode (one
worker thread waking ~855 times/s, car, 2026-11-24). So after 60 s of `pause`,
on `video 0`, and at `eof` (its own flush), ytdec ends the session but stays up:
the MGVF file keeps its last frame on screen, and stdin and the stats stay as
they were. `resume` after an idle close seeks to the paused pos (a new session
from the keyframe before it, up to one GOP decoded unseen: ~0.3-1 s); `seek` and
`video 1` seek anyway. No command or event changed.

stdout, JSON lines:

```
{"ev":"ready","w":1280,"h":720,"fps":30.000,"src_w":1280,"src_h":720}     w,h = --rect (the MGVF size until the first frame)
{"ev":"stats","decoded":N,"shown":N,"dropped":N,"late_ms":x,"cpu_pct":x,"fps":x,"pos_ms":N}   every 2 s
{"ev":"eof"}
{"ev":"error","msg":"..."}
```

`decoded`/`shown`/`dropped` are totals since start; `fps` is frames shown a
second over the last 2 s; `late_ms` the mean (pos − pts) of the frames shown
in it (≈ −8: shown 8 ms ahead); `cpu_pct` this process, % of one core.
`eof`: every indexed frame shown or dropped, and `video.h264` exists under
its final name (the fetcher writes no other marker). A `.part` that stops
growing is a stall (ytd.js shows buffering), never the end. ytdec stays up after `eof` (a `seek` plays again).

Exit codes: 0 quit, 1 decoder init failed, 2 bad arguments / MGVF file, 4 the
index's codec cannot be decoded here (an `error` event first: the VP9 plugin
did not load or register, the first frame failed, or 30 records gave no
frame; ytd.js then fetches the video again as H.264), 70 fatal signal or
exception.

VP9: `dlopen("/usr/lib/mediasdk/libmfx_vp9d_hw64.so")` once, then per MSDK
session `CreatePlugin(a922394d8d87452f878c51f2fc9b4131)`, `GetPluginParam`,
`MFXVideoUSER_Register(session, Type)`, as the MSDK dispatcher's
`MFXVideoUSER_Load` does (`Unregister` before `MFXClose`). Checked offline under
the unit's ld.so on the rootfs copy: `CreatePlugin` and `GetPluginParam` (type
1 = decode, codec `VP9 `, API 1.24; a VP8 UID is refused); registering it in
a software (libmfxsw64) session returns `MFX_ERR_UNSUPPORTED`. Registering on
the GPU, decoding, opaque surfaces into VPP, seeks and superframes are
car-only. A superframe is fed whole; if the plugin takes only its hidden frame
and answers MORE_DATA with the rest left in the bitstream, the decoder calls
it again on the rest.

## MGVF output

`/dev/shm/mgu-video-ridetube`: created with `O_NOFOLLOW`, 0600, `fchown` to
`nobody` (a warning in the log when not root), 64-byte header + 3 slots,
BT.709 limited range. The writer fills slot (slot+1)%3, then stores slot,
then seq+1 (release). The frame size follows the decoder's frames (with
`--no-upscale` they are smaller than `--rect`): the file is truncated and
re-extended at the new size, seq carried on (a new header with seq 0 would
make the renderer read a black slot), then the frame is written. The file is left in place on exit (the app drops the
video). A restart truncates and re-extends the same inode: safe, mgu-render
reads it with `pread` (gfx.c), never `mmap`, so a shrink cannot SIGBUS it.

Running as `nobody` (ytd.js): `/dev/dri/renderD128` is `root:video 0660`, so
the unit needs `SupplementaryGroups=video` or `init` fails (exit 1); a
root-owned `/dev/shm/mgu-video-ridetube` left by a root run blocks a `nobody`
ytdec (exit 2); `fchown` to nobody only works as root, otherwise the file
stays the caller's uid, which mgu-render also accepts.

## Crash rules

`prctl(PR_SET_DUMPABLE, 0)` and `RLIMIT_CORE 0` first; SEGV/BUS/FPE/ILL/ABRT/
QUIT/TRAP/SYS on a `sigaltstack` write one line and `_exit(70)`;
`std::set_terminate` → `_exit(70)`; `main` in try/catch; built `-DNDEBUG`.

## Measured on the unit (2026-11-02, 30 s runs, fake clock, nothing displaying the file)

| Sample | Output | fps shown | drops | ytdec CPU | RSS | system |
|---|---|---|---|---|---|---|
| 720p30 Main, no B (3 Mbit/s) | 1280x720 fit | 30.0 | 0 | 17 % of a core | 28 MB | 158 → 214 % of 400 |
| 1080p25 High, B frames | 1280x720 fit | 25.0 | 0 | 15 % of a core | 31 MB | 159 → 215 % of 400 |

Without GPU copy (`YTDEC_GPUCOPY=2`) an MSDK thread copying VPP's output from
the tiled GPU surface takes ytdec to 65 % of a core at 720p. With decoding
running, the unit's other busy processes (natp, vsomeipd, AVB, ALSA routing)
also use ~30 % more CPU than idle (they did not with ytdec up and `video 0`),
so the whole-system cost is ~55 % of one core, not 15 %.

A 1080p frame at pts 20000 captured from the file after `seek 20000`
matches ffmpeg's display frame 500 (mean |ΔY| 0.3; neighbours 1.0+): display
order, timestamps and planes are right.

## Not done

- VP9 is checked on the unit only (no software VP9 to compare with): mgu-yt's ytdec
  with this code played a 720p60 VP9 video for 30 s there at ~26 % CPU.
- The renderer never displayed the file (no app yet).
- No catch-up seek: a clock far ahead of the video (without `seek`) is caught
  up by decoding without VPP (~450 fps at 720p) rather than jumping.
- An IDR without its own SPS is not a seek point (h264_mp4toannexb repeats
  SPS/PPS before every IDR, so this does not happen with the fetcher's streams).
- Colour matrix is always BT.709 limited (YouTube's); the stream's VUI is not read.
