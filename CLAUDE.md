# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

dltools is a set of command line utilities for Blackmagic Decklink cards, written in
C++ against the Decklink SDK. The primary tool is `dlplay`, which decodes a video
file or network stream and plays it out of the card as an SDI source. Also included:
`dlcap` (capture raw YUV from SDI), `dlinfo` (enumerate cards and supported modes)
and `dlskel` (template for new tools).

## Build

Plain Makefile, no configure step and no test suite.

```sh
make                # optimised build of all tools
make debug          # same objects, no -O2/-ffast-math/-fomit-frame-pointer
make clean          # required when switching between all and debug (see below)
make dlplay         # build a single tool
sudo make install   # installs to $(PREFIX)/bin, default prefix /usr/local
```

`all`, `debug` and `depend` differ only in `CXXFLAGS` but write to the same `.o`
files, so make will not rebuild when you switch between them. Always `make clean`
first.

The Decklink SDK is expected at `/usr/local/decklink/include` (`SDKDIR` in the
Makefile); `DeckLinkAPIDispatch.cpp` is compiled out of there into the build.

The dependency list at the bottom of the Makefile is hand-pasted output from
`make depend` (which just adds `-MMD`). If you add an include, regenerate and paste
it back, or the incremental build will miss header changes.

### Feature flags

Set at the top of the Makefile, not via configure:

| Flag | Default | Effect |
|---|---|---|
| `LIBYUV` | 1 | `-DHAVE_LIBYUV`, links `-lyuv -ljpeg` |
| `HEVC` | 1 | `-DHAVE_LIBDE265`, links `-lde265` |
| `FFMPEG` | 1 | `-DHAVE_FFMPEG`, links avcodec/avformat/avutil |

**`FFMPEG=0` means H.264 and AV1 do not decode.** Those paths become `dlexit("no
support for ... in this build")`, in both the elementary-stream and transport-stream
dispatch, so the README's claim of H.264 support only holds while ffmpeg is enabled.
MPEG-2 falls back to libmpeg2 and HEVC to libde265 when ffmpeg is off, so an
`FFMPEG=0` build still plays MPEG-2 but nothing else beyond raw YUV.

The ffmpeg code targets the current API (verified against ffmpeg 8.0): the decoders
use `avcodec_send_packet`/`avcodec_receive_frame`, and `libavcodec/avcodec.h` must be
included explicitly in `dldecode.h` because `avformat.h` no longer pulls it in.

## Architecture

### Three-layer pipeline

Everything in `dlplay` flows through three virtual base classes, each in its own
file, wired together in `main()` in `dlplay.cpp` based on an autodetected
`filetype_t`:

```
dlsource  (dlsource.h)  transport:  dlfile / dlmmap / dlsock / dltcpsock
   |  raw bytes
dlformat  (dlformat.h)  container:  dlformat(raw) / dlestream / dltstream / dlavformat
   |  elementary stream bytes + PTS/DTS
dldecode  (dldecode.h)  codec:      dlyuv / dlmpeg2 / dlhevc / dlffvideo / dlmpg123 / dlliba52 / dlpcm
   |  UYVY or v210 written straight into a Decklink frame buffer
IDeckLinkOutput
```

Each layer is attached to the one below with `attach()`. Adding a codec or a
container means subclassing the relevant base and adding a case to the dispatch
switch in `dlplay.cpp` (around the `switch (filetype)`).

### Source readers

`dlsource::attach()` returns another `dlsource *` — an **independent reader** on the
same data, with its own read position, EOF and error flags and read buffer. `dlfile`
implements a reader by reopening the file, one fd each; `dlmmap` shares the mapping
and gives the reader its own pointer into it. This is how the video and audio decoders
walk the same transport stream file at different offsets simultaneously: there is one
`dltstream` format filter per pid, and each holds its own reader.

Readers belong to the source that created them and are deleted with it, so a
`dlformat` never frees the reader it holds. `dlsock::attach()` returns `this`, since a
socket has a single read position — two format filters on one socket therefore steal
each other's packets (see `BUGS`), which is why a demux is the right long term answer
for network input.

`dlplay` probes with the source object itself (`find_pid_for_stream_type`, then
`source->rewind()`) before any format filter is attached, so the probe never disturbs
a reader.

File input is memory mapped (`USE_MMAP` at the top of `dlplay.cpp`, on by default);
build with it commented out to fall back to `dlfile` and ordinary `read()`.

### Looping

Input looping is not in the playout loop — it is in `dlformat::read()`, which rewinds
its own reader and re-reads when a read comes up short. The known bug in `BUGS`
(MPEG-1 audio not looping with video in a TS) lives in this interaction between the
per-reader rewind and the timestamp handling in `dltstream`: each stream loops
whenever it happens to hit the end of the file, independently of the other.

### Decoder probing

`dldecode::attach()` **consumes input** — decoders parse forward looking for a
sequence/parameter-set header and fill in their public `width`, `height`,
`interlaced`, `framerate` and `pixelformat` members. `dlplay` reads those fields
afterwards to choose the Decklink display mode. Members are deliberately public
("we're not designing a type library here"); follow that convention rather than
adding accessors.

`decode()` writes converted pixels directly into a caller-supplied Decklink buffer
and returns a `decode_t` carrying the size and the timestamp.

### Timebases

- `pts_t` — 90 kHz, as it comes out of the transport stream.
- `sts_t` — 180 kHz system timestamp, used for all Decklink scheduling. Every
  `ScheduleVideoFrame`/`ScheduleAudioSamples` call passes a timescale of `180000`.

### Pixel formats

Decoders produce planar YUV (`I420`/`I422`/`I444`, or `YU15`/`YU20` for 10-bit) and
`dlconv.cpp` packs it to what the card wants: `bmdFormat8BitYUV` (UYVY, rowbytes
`width*2`) or `bmdFormat10BitYUV` (v210, rowbytes `((width+47)/48)*128`). libyuv is
used where available with hand-written fallbacks in the same file. Field-based
variants (`convert_top_field_*`, `convert_bot_field_*`) exist for interlaced output.

### Playout, threading and buffers

`dlplay` runs three concurrent things: the main thread decodes and schedules frames;
`callback::ScheduledFrameCompleted` posts a semaphore that the main loop waits on;
and a separate pthread runs `display_status` for the on-screen statistics line.

Playback prerolls `PREROLL_FRAMES` (60) frames before `StartScheduledPlayback`. A
history buffer of `PREROLL_FRAMES*3/2` completed frames is retained so pause mode can
step backwards — it must stay larger than the preroll depth or stepping breaks.

Frame memory comes from `dlalloc`, a custom `IDeckLinkVideoBufferAllocator` (the SDK
14.3+ interface, replacing the old memory allocator) handing out a fixed pool of 256
`dlvideobuf` objects.

### Interactive keys in dlplay

Handled inline in the playout loop under `USE_TERMIOS` via the `dlterm` class:
`p`/space pause (then `j`/`k` or left/right arrows to step frames), `s` toggle
deinterlace field order, `v`/`V` adjust verbosity, `q`/Return/Esc quit.

## Conventions

- C-style C++: no exceptions, no smart pointers, `malloc`/`free`, `printf`-family
  output. Match it.
- All diagnostics go through `dlmessage`/`dlerror`/`dlexit`/`dlapierror`/`dlstatus`
  in `dlutil.cpp`, never bare `printf`. Gate chatty output on the `verbose` level.
- Every Decklink `HRESULT` is checked; report failures with `dlapierror`.
- Comments are lowercase `/* ... */`, one line, above the block they describe.
- Numeric command line arguments are parsed with `parse_int_arg` from `dlutil.cpp`,
  never `atoi`/`strtol` directly:

  ```c
  vid_pid = parse_int_arg(optarg, 2, 8191, "video pid");
  ```

  It rejects non-numbers, trailing junk, overflow and out-of-range values, calling
  `dlexit` with the option name and the offending string, so no separate range check
  is needed at the call site. The `name` argument is a human-readable noun phrase
  that gets embedded in the error message. Parsing is base 10, so hex is not
  accepted. Use bare `strtol` only where trailing text is legitimate, such as the
  first-octet multicast test on a dotted-quad address in `dlplay.cpp`.

## Testing

There is no automated test suite. Changes are verified by playing sample transport
streams out of a card. Several large sample streams sit untracked in the working
directory (MPEG-2, H.264 4:2:2 10-bit, and a CXP conformance stream) — leave them
untracked and do not add them to git.
