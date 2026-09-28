# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

dltools is a set of command line utilities for Blackmagic Decklink cards, written in
C++ against the Decklink SDK. The primary tool is `dlplay`, which decodes a video
file or network stream and plays it out of the card as an SDI source. Also included:
`dlcap` (capture raw YUV from SDI), `dlcard` (play a static test card, currently
75% colour bars in 8-bit UYVY, default 720p59.94), `dlsync` (end-to-end latency and
lipsync through an external chain), `dlinfo` (enumerate cards and
supported modes) and `dlskel` (template for new tools).

`dlcard` draws every combination of its two static overlays (`-o` centre text, which
defaults to the card's model name, and `-m` video mode) into its own background buffer
up front. Each output frame is a copy of the current background into the next frame of
a fixed ring of `PREROLL_FRAMES+2`, plus, with `-t`, the timecode for that frame number
blitted from prerendered glyphs and set in the SDI metadata (RP188, or VITC for SD). The
ring is safe to reuse because a frame is only scheduled after a completion, and it is
even so each frame is always odd or always even, which keeps the VITC1/VITC2 choice at
high frame rates from leaving a stale timecode on a reused frame. The `o`/`m`/`t` keys
change what the next frame gets, so a toggle appears after the preroll depth.
Timecode is drop frame at 29.97 and 59.94 (flag on every frame, `;` on screen); 23.98
has no drop frame form and counts non-drop.

`-a` adds a middle C tone at -20 dBFS on every audio channel. Like the timecode it is computed from its
sample number, so a partial `ScheduleAudioSamples` write needs no leftover buffer: the next
block starts from the first sample not written. It is scheduled with a timescale of 48000
and the sample number as the stream time, the one exception to the 180kHz convention,
because a sample is 3.75 ticks of 180kHz. It is kept 250ms ahead of playback, which also
bounds the latency of muting with `a`, and once `-n` has requested the stop it is only
topped up to the end of the last frame, since the buffer level cannot be read after
playback has stopped.

`dlsync` is a copy of `dlcard`'s output (bars, burnt-in and RP188/VITC timecode, tone)
with an `IDeckLinkInput` on the same card, so it needs a full duplex card (`dlinfo` reports
the duplex mode). Each received frame's timecode is turned back into the frame number it
was sent as (`timecode_to_frame`, the inverse of `frame_to_timecode`, unwrapped to the
nearest day), whose send time is its number times the frame duration in output stream
time. At high frame rates a frame carrying VITC2 rather than VITC1 is the odd one of its
pair. In SD the timecode is sent as VITC and, where the card can (the 4K models), as RP188
too, and read back as whichever arrives, VITC first: the enc->dec chain tested passes only
the RP188 (ancillary timecode) in 576i and drops VITC. A change of display mode in the chain wakes the main loop, which stops and exits
with an error. Capture passthrough is disabled so the output never follows the input.

The latency is the frame's `GetHardwareReferenceTimestamp` less its send time. On the 4K
Extreme 12G the frame timestamps are on the **input's** hardware reference clock, whose
base differs from the output's by days, so the output stream time is related to the input
clock, not the output clock, by reading input clock, stream time, input clock and taking
the midpoint. The stream time drifts about 46 ppm against that clock, so the main loop
publishes the tightest sample of each second to the callback rather than measuring once.

The picture (bars or black) and the tone are on for `CYCLE_SECONDS` (10) s of frames (`10*fps`, so 10.01 s
at 59.94) and off for the same, `frame_is_on()`. It was 30 s, cut to 10 s to see results sooner. The tone's gain is a pure function of the
sample number, `tone_gain()`: a 5 ms linear ramp centred on the exact time of the first frame
of each run, which falls between samples at 59.94. Received audio samples are timed on the
input's clock through the stream time of the video frame they came with. The level is the
rectified mean over one tone period, not RMS, because RMS over a linear ramp crosses 50%
about 0.23 ms early on the rise and late on the fall; the residual error is ±0.2 ms,
depending on the tone's phase at the edge. The threshold is half the measured on level, not
the nominal -20 dBFS, so a chain which changes the level still measures correctly. An audio
edge is numbered by the video latency and matched to the arrival of the frame whose
timecode is that edge. Lipsync is audio minus video, positive when the audio is late. The
mean luma of each frame is only a check against its timecode.

`--calibrate`, run on a loopback cable, saves the mean latency and lipsync as the card's own
to `~/.dlsync` (or `-f`), one line per mode keyed by `describe_display_mode`, e.g.
`720p5994 video 16.661 lipsync 0.123`, and normal runs subtract them. Lipsync needs the off
edge at 10 s and the on edge at 20 s; a calibration stopped before any edge keeps the
lipsync saved before.

At startup `IDeckLinkStatus` gives the mode the card is still sending from its last use
and the mode detected on its input. If both are the mode about to be sent, the chain is
already in it and measurement starts at once. Otherwise the chain has to change mode,
which restarts it: it can take tens of seconds, drop its signal several times and hold a
false steady latency for several seconds on the way. So until it has settled, a change of
mode is waited for rather than fatal, nothing is measured, and the status line says what
is being waited for. It has settled once the latency has stayed within half a frame for
`SETTLE_SECONDS` (10). After that a mode change is fatal. The lipsync of the encoder and
decoder tested is different after each restart (+58, +75, +77, +96 ms at 720p59.94) but
steady within a run to 0.15 ms, while its latency returns to within 1 ms. Each stereo pair
can be encoded with a different audio codec, so pairs can differ in lipsync by tens of ms. After a mode
change the first lipsync measurement is left out (`discard_first`) as a precaution, which
delays the first result by 10 s; so far the one left out has matched the next.

Messages to the user speak of the "enc->dec chain", the "end-to-end latency" and the
"lipsync" per channel, never of edges or the picture and tone turning on or off: each
lipsync measurement is reported as one line with the latency and both channels, and the
report gives each channel's lipsync over the run. A pair of channels whose values agree to the
digits shown is written once, `[1-2: +74.60, 3-4: +74.60]`. The report puts the mean, min and
max on three lines in aligned columns, joining a pair only if it agrees in all three. The status line shows the last latency
and lipsync, then either a note (`no timecode`, `timecode not advancing`, `timecode ahead
of the output`) or `next measurement in N s`, the countdown to the next edge. Latency is
from the first arrival of a frame, so a frame whose timecode does not advance (a decoder repeating a frame) is left out and counted, as is one whose latency would
be negative, which means the chain replaced the timecode. Frames without timecode before the
first one with it are the decoder's output from before the first frame came back, and are
not reported as dropouts. The final report warns about any of these, about a picture which
disagreed with its timecode, and about a channel with no edges or a tone level more than
1 dB from -20 dBFS.

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
| `FREETYPE` | 1 | `-DHAVE_FREETYPE`, links freetype and fontconfig into `dlcard` only, for its `-d`/`-m` text |

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
get sample frames of the decoders' stereo 16-bit output.

### Audio channels

`dlplay`, `dlcard` and `dlsync` output `AUDIO_CHANNELS` (8, in `dlutil.h`) channels of
audio, with no option yet to change it. `audio_channels()` in `dlutil.cpp` reduces that to
the most the card reports in `BMDDeckLinkMaximumAudioChannels`, from the counts the SDK
accepts (2, 8, 16, 32, 64), so a card with 16 or more is ready for a larger count and a
card with fewer falls back with a message. For now every pair carries the same audio:
`dlplay` copies each decoded stereo frame into every pair with `duplicate_stereo()`, into a
separate output buffer, so the scheduling offsets are in frames of `aud_channels*2` bytes;
`dlcard` and `dlsync` write the tone into every channel, from a block buffer allocated for
the channel count. `dlsync` captures as many channels as it sends, measuring lipsync in
each, with its arrays sized for `MAX_AUDIO_CHANNELS` (64). Changing the channel count on
the SDI makes the enc->dec chain tested restart, like a change of mode.


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

3:2 pulldown (soft telecine) is not handled, see BUGS. Nothing reads `repeat_pict`
(or libmpeg2's repeat_first_field), so each coded frame is scheduled for one frame
period. ffmpeg reports such a stream as progressive 29.97, and it plays as 1080p29.97
with its frames reported late. The card cannot repeat a field, because
`ScheduleVideoFrame` works in whole frames of the display mode and an interlaced frame
always carries both of its fields. A fix therefore has to weave the frames in software
in `dlffvideo::decode()`:

- queue the decoded fields, two or three per coded frame, taken from `repeat_pict` and
  `AV_FRAME_FLAG_TOP_FIELD_FIRST`;
- time each field at the frame's PTS plus 3003 (180kHz) per field;
- build each output frame from the next top and bottom field;
- report the stream as interlaced 29.97.

The simpler alternative is to ignore the flag and play the coded frames in a
1080p23.98 mode.

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
