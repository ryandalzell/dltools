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
and gives the reader its own pointer into it; `dlsock::attach()` returns `this`, since
a socket has a single read position.

Readers belong to the source that created them and are deleted with it, so a
`dlformat` never frees the reader it holds.

`dlplay` probes with the source object itself (`find_pid_for_stream_type`, then
`source->rewind()`) before any format filter is attached, so the probe never disturbs
a reader.

File input is memory mapped (`USE_MMAP` at the top of `dlplay.cpp`, on by default);
build with it commented out to fall back to `dlfile` and ordinary `read()`.

### Transport stream demux

`dldemux` (in `dlts.cpp`) is the only thing that reads a transport stream. It holds
one reader and, on demand, pulls 188-byte packets and assembles whole PES packets into
a queue **per pid**; packets for pids nobody registered are discarded. Each
`dltstream` format filter is a consumer of one pid: `dltstream::read()` pops the next
PES packet, and if that pid's queue is empty the demux reads more input, queueing the
other pids' packets as it goes. Reading is pull-driven, single threaded, no read-ahead
thread.

This is what lets the video and audio filters be far apart in the file but together in
time from a single read position, which in turn means a transport stream works from a
socket, not just a seekable file. One `dldemux` is created in the `TS` case in
`dlplay.cpp` and shared by both filters; `dltstream::attach(dlsource *)` makes a
private demux for a single pid, for callers outside `dlplay`.

A queue holds only the skew between the pids, which is well under a second in a sane
mux, but at high bitrates that is still a lot of bytes: half a second of 97Mbps video
queued 6.4MB while waiting for an audio pid in bostonvchicago. `MAX_QUEUE_BYTES` (32MB)
caps it: past that, the oldest packets for that pid are dropped with one warning, which
means a consumer has stopped reading or the mux is badly skewed.

`find_pid_for_stream_type`, also in `dlts.cpp`, is what chooses the pids: it reads the
PAT and PMT and returns the first pid carrying one of a list of stream types, along
with the type it found, which is how `dlplay` picks a decoder. Stream type 0x06 is
private data and does not name a codec, so the descriptors of that elementary stream
are consulted: a DVB AC-3 descriptor or an `AC-3` registration reports 0x81 (so DVB
AC-3 audio reaches `dlliba52`, not `dlpcm`), a `BSSD` registration reports 302M, and a
codec with no decoder here reports a stream type which is not in the list, so the pid
is skipped and the search goes on. Private data with nothing to identify it is still
assumed to be SMPTE 302M.

### Looping

For elementary streams, looping is in `dlformat::read()`, which rewinds its own reader
and re-reads when a read comes up short.

Raw YUV can instead loop over a range of frames, given with `-a` and `-n`: `dlyuv` counts
the frames it has read and seeks its format back to the first one (`dlformat::seek()`,
which only `dlfile` and `dlmmap` implement), so for raw YUV `-n` is the length of the
loop rather than a limit on the playout.

For transport streams the demux loops the input itself: at end of input it rewinds the
one reader, discards the partly assembled packets and carries on, so every pid loops
at the same point. Because the scheduler in `dlplay` needs monotonic timestamps (and
exits on any that go backwards — see the loop debugging block in the playout loop),
`dldemux::rebase_timestamp()` adds an offset to every PTS and DTS so that a new pass
through the input continues from the highest timestamp of the previous pass. The
offset is common to all pids, so their relative timing is unchanged across the loop.

The data either side of a loop is not continuous, and a decoder which is holding part
of a frame will otherwise join the two and emit a frame with the wrong timestamp. So
the first PES packet of each pid in a new pass is marked, `dlformat::discontinuity()`
reports it for the packet just read, and a decoder throws away what it has buffered:
`dlffvideo` re-initialises its parser and flushes the codec, `dlmpg123` re-opens its
feed, `dlliba52` drops the part of an AC-3 frame it holds and syncs again, and `dlpcm`
drops a partly filled AES3 packet and starts the next one. `dlmpeg2` and `dlhevc`, which are only used when
the ffmpeg decoders are compiled out, do not do this yet.

### Decoder probing

`dldecode::attach()` **consumes input** — decoders parse forward looking for a
sequence/parameter-set header and fill in their public `width`, `height`,
`interlaced`, `framerate` and `pixelformat` members. `dlplay` reads those fields
afterwards to choose the Decklink display mode. Members are deliberately public
("we're not designing a type library here"); follow that convention rather than
adding accessors.

`decode()` writes converted pixels directly into a caller-supplied Decklink buffer
and returns a `decode_t` carrying the size and the timestamp. `decode_t::size` is
a count of **bytes** for audio as well as video, so `dlplay` divides it by four to
get sample frames for a stereo 16-bit output.

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

Playback starts at the timestamp of the first video frame. The audio of a transport
stream normally starts a little before the video, and a gap at the start of the video
output makes the card report every frame of the playout as displayed late, so the
audio ahead of the first frame is given up instead: the card discards samples
scheduled before the start time, which keeps everything after it in sync, and `dlplay`
reports how much audio that was at verbosity 1. A video with no timestamps of its own,
where the audio has them, starts with the audio instead.

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
