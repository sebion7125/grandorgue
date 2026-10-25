**Unofficial demo snapshot** of a personal GrandOrgue fork. The version string is a placeholder marking an internal build, not an official GrandOrgue release.

This is a source-only snapshot of ongoing work on the sample codec, real-time dropout concealment, audio device handling, and the sound scheduler. See the `demo` branch for the full source.

## V3 Codec — fixed-width planar predictive codec with batched SIMD decode

A lossless audio sample codec designed for the specific case this project needed: many independently-positioned compressed streams decoded concurrently on SIMD hardware (a polyphonic sampler). Conventional variable-length per-sample codecs decode with a data-dependent branch per sample, which defeats vectorization.

The codec instead divides each channel into fixed-length blocks, predicts each sample with one of four fixed-order integer linear predictors, and stores the residuals at a single fixed byte width per block (chosen from that block's own peak residual), in a channel-planar layout. Because the layout is fixed-stride and branch-free, up to 8 independent streams can be decoded together in one AVX2 pass via a two-stage extraction-then-transpose kernel — one lane per stream, even when the streams are mutually unaligned. A resumable-snapshot mechanism lets any stream begin decoding at an arbitrary position without decoding from the start.

Measured on a corpus of 4000+ real audio files, the technique reached 4.75x the decode throughput of a representative reference codec at a better compression ratio (~1.53x vs ~1.43x), under controlled lab conditions. On real two-channel material, the format and the batching were measured as separate contributions (~1.0–1.9x and ~2.4–2.7x respectively); application overhead eats into both figures, which is why this project kept measuring rather than stopping at the lab number.

Published as an independent, dual-licensed (GPLv3-or-later / commercial) library: [sebion7125/v3codec](https://github.com/sebion7125/v3codec). Full technical disclosure (written to establish prior art and to be reproducible independent of this host application) is included in that repository and was submitted to Technical Disclosure Commons.

## Xorcist — spectral-continuation concealment for missed real-time deadlines

A technique for concealing missed audio processing deadlines ("dropouts"/"xruns") by synthesizing a plausible continuation of the signal instead of outputting silence or a glitch. A short-time Fourier analysis runs continuously over recent signal history; when concealment is needed, each analysis bin is continued independently by extrapolating its phase and amplitude, and — for bins containing two sinusoidal partials — by a fitted two-pole autoregressive model, generalizing the classical single-rotator phase-vocoder freeze (which is exact only for one partial per bin).

A derived aliasing limit constrains the usable analysis hop for the two-partial model; a cheap closed-form stability test runs before falling back to explicit root-finding only when needed. The continuation is spliced onto the real signal using one of two seam-repair strategies, chosen per occurrence by a spectral-similarity measurement. A frequency-damping term suppresses a positive-feedback failure mode that otherwise arises when previously-concealed audio is itself re-analyzed. An optional second, short-window model instance runs in parallel and takes over newly-attacked content, because a long window's history stays contaminated by pre-attack content long after a new note begins.

Published as an independent, dual-licensed (GPLv3-or-later / commercial) library: [sebion7125/xorcist](https://github.com/sebion7125/xorcist), with its own technical disclosure submitted to Technical Disclosure Commons alongside the V3 codec's.

## MIDI auto-archiver (Pianoteq-style)

Automatically records all MIDI input to a configurable archive folder with timestamp-based filenames, completely independent of — and never interfering with — the existing manual recorder. Starts on the first note, stops after a configurable inactivity timeout (default 8s). A 1-second pre-roll of silence is inserted before the first note so that note durations in the archived file stay correct even though the recorder started mid-note. SysEx state dumps and MIDI player playback are isolated from the auto-archiver so they can't trigger or pollute an archive recording.

## Audio device robustness

Three separate, hard-to-reproduce bugs found via live testing on a real ASIO device (ESI Gigaport eX) plus Application Verifier heap diagnostics, now fixed:

- **COM apartment-threading hang/crash**: PortAudio's WASAPI/ASIO backends and RtAudio's ASIO backend initialize COM on whatever thread first touches them. The previous implementation spawned a fresh thread per open/close call, so an Open and a later Close could land on different COM apartments, causing hangs on close and crashes on reopen. Fixed by routing all device open/close/enumerate calls through one persistent worker thread.
- A hang in `abortStream()` when closing a port whose device was already known to be lost.
- Automatic reconnection when an audio device disappears and later becomes available again (500ms retry interval), plus suspend/resume handling on Windows (`wxPowerEvent`).

## Scheduler rework: two-phase decode/render callback

Each audio period callback is now split into a decode phase and a render phase instead of one combined pass. In the decode phase, every active voice's exact source span for that callback is collected up front, duplicate work across unison/coupled voices is eliminated, and the resulting decode work is coalesced into same-channel-count batches that worker threads claim via a single atomic fetch-add each (no collisions, no locking). The render phase is otherwise unchanged. This removes redundant per-voice decode work that the previous single-phase design could not detect, and hardens the group task's internal bookkeeping (double-buffered, epoch-disciplined, matching the existing sampler-array synchronization).

## Audio Monitor

A new diagnostics dialog: per-channel polyphony and CPU-load graphs over time, an automatic ceiling-sweep benchmark that finds the polyphony level at which the engine starts missing its deadline, and live V3-codec decode counters — intended to answer performance and glitch questions with a graph instead of a guess.

## Also included

Assorted smaller fixes accumulated alongside the above: crossfade/fader correctness fixes, and tooling for release-align (crossfade splice point) analysis.

## Status

Experimental, unsupported fork snapshot — not an official GrandOrgue release, and not yet reviewed for merge into the upstream project. Built and CI-tested across Linux (x86-64, armhf, aarch64), macOS (Intel, Apple Silicon), and Windows (cross-compiled) via this repository's own GitHub Actions workflow.
