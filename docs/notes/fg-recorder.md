# FG event recorder (fg-recordings)

While `[FramePacing] FgRecorder` is true (Frame Gen tab, Frame pacing card; off by default), the DLL writes
one JSONL file per session next to itself, in `fg-recordings\`. Each line is one event: presents on the FG
chain and the visible output (with HRESULT and duration), Reflex sleeps, the hidden DX11 present, AutoMFG
lines, NR model rebuilds, provider activations and teardowns, DLSS-G preset switches, provider module loads,
XeFG factor changes and the user's marks. A `session.begin` line opens the first file (fork version, exe name,
build match, settings summary, QPC frequency); rotations open with a `continuation` line. A `session.end` line
closes a clean stop; a recording without it is unfinished. No record holds a path, a user name or a machine
name: modules are named by file name only.

- Bounds: the queue is capped (overflow is an explicit `records_dropped` line, never silent loss), each file
  rotates at 16 MiB keeping **base + .1 to .8, 9 files = 144 MiB per session**, and only the newest 5 sessions
  are kept (up to 720 MiB on disk while five recordings exist). The sweep matches the fork's own file-name
  pattern and nothing else. 144 MiB is about 40 minutes at 60 base fps and 2x frame generation (the 3.4 MB/min a
  measured 2x session wrote), and each file holds 16 MiB plus the queue batch that crossed the limit (1.4-2.3 KB
  measured, at most one batch), so the bound depends on the rate the session writes: at 3x/4x output or 120 base
  fps it roughly halves, and the analyzer's `head_rotated` flag stays the honest backstop for a truncated head. A
  25-minute play session keeps its beginning, which is where a problem usually starts. The
  alternative considered and rejected: aggregating the `out.present.*` records per second would shrink the
  volume by about 60 % but destroys the per-output accounting the analyzer's batch and generated-frame figures
  are built from.
- Cost: with the key off each tap is one relaxed atomic load plus a thread-local counter, and every 16th tap
  per thread re-reads the config; there is no thread, no allocation and no file. With it on, callers only
  enqueue; a below-normal-priority writer thread does the disk I/O about every 100 ms and asks the device to
  flush at most about once a second (a flush per batch throttles the game's own disk I/O, and data already
  written survives a process crash without one). Quitting writes the queued records and `session.end`
  synchronously on the exiting thread (bounded: at most the queued records, never waits on the writer
  thread). Turning the key off ends the session too: the worker drains first, then stops.
- Format: strict JSON per line (`seq`, `qpc`, `tid`, `frame`, `gen`, `prov`, `kind`, `a`, `b`, `c`, `msg`).
  Providers: 0 none, 1 DLSS-G, 2 XeFG, 3 FSR FG. Present durations (`b` of the `*.end` lines, sleeps and the
  hidden present) are QPC ticks; multiply by 1000/`qpc_frequency` for milliseconds. The durations are
  wider than the label: `fg.present` spans the FG dispatch, the Reflex sleep and the frame-limiter sleep,
  and `out.present` includes the deferred release and the device-removed logging, so a long present alone
  never names the cause.

## The user's marks

The one-key mark (`[FramePacing]` `FgRecorderMarkKey`, unbound by default; bind it in the Keybinds section of
the overlay) writes, per press, one `mark` record with `a` = the mark number and one line in `OptiScaler.log`,
`user mark N`. It is how a user says "now" while playing: the analyzer lists every mark with its number and
the second of the session it fell in, so a clip, a hitch or a visible artifact can be found again in the
recording without guessing timestamps. With the recorder off a press only writes the log line; the numbers
keep counting across a key-off/key-on and across sessions of one game run, so two files of one run can be put
in order.

## How to record and read a session

1. Tick the checkbox (or set `FgRecorder=true`), play, quit normally so `session.end` is written. Bind the mark
   key first if you want to point at moments.
2. Run the analyzer on the session's base file (it reads the `.1` to `.8` rotations in order):
   `pwsh -NoProfile -File tools/analyze-fg-recording.ps1 fg-recordings/fg-rec-<stamp>-<pid>.jsonl`
3. Exit 0 means clean; exit 2 means records were lost (overflow, or a session head lost to rotation),
   a line was malformed or the session never ended (a crash loses the records still queued, up to about
   100 ms); exit 3 means the session is still open (the writer holds the file: quit the game or turn the
   key off, then analyze it); exit 1 is a usage error.

## What to attach to a bug report

The full session (the base file plus its `.1` to `.8` rotations), the analyzer output, `OptiScaler.log` and the
ini. Keep them even when gameplay looked normal: the recording is the evidence, not the verdict. A long
present alone does not establish a cause; compare warnings, input gaps, sleep timings and provider
switch generations around the same QPC timestamp.

## Limits

The recorder watches the present path: bridge device-removed early-outs above the FG present are not
separate events (the failing present's HRESULT still is), FSR FG has no activation event (its presents
still carry provider 3), and the game version stamp stays in `status.json`/`OptiScaler.log` rather than
being duplicated here. Read a session after quit or key-off: the writer holds the live file open,
and an analyzer run over a copy taken mid-session reports what the copy holds, honestly.
