# Shotcut Agent API

The Shotcut **Agent Server** is an opt-in JSON-RPC 2.0 endpoint over a single
WebSocket connection that lets an external program — typically an LLM-driven
agent — observe and edit a running Shotcut project while the user keeps the
GUI open.

It is off by default. Turn it on either via:

* **Settings → AI Agent…** (a checkbox, port, optional bearer token), or
* the `--agent-server[=PORT]` command-line flag (default port `5555`,
  loopback only).

The default bind address is `127.0.0.1`. To accept non-loopback connections
you must explicitly tick **Allow remote connections (insecure)** *and*
configure a bearer token.

## Compile-time disable

Shotcut ships with the agent server compiled in. To produce a build without
it, configure with `-DSHOTCUT_ENABLE_AGENT_SERVER=OFF`.

## Wire format

* One JSON object per WebSocket text frame.
* JSON-RPC 2.0 — every message has `"jsonrpc": "2.0"`.
* Requests carry an `id`; notifications (no response expected) omit it.
* Server-pushed events are JSON-RPC notifications (no `id`).

## Authentication

When connecting, present the bearer token in the upgrade request:

```
GET / HTTP/1.1
Host: 127.0.0.1:5555
Authorization: Bearer <token>
```

The server also accepts the token on every message (`{"auth": "<token>", ...}`).
Loopback peers may omit the token only when no token is configured. Remote
peers always need a token.

## Single-client policy

The server accepts one client at a time. A second connection attempt is
closed with the policy-violated code `1008` and the reason
`"agent server is single-client"`. This is intentional for v1 to keep
mutation semantics simple.

## Standard error codes

| Code      | Meaning                                |
|-----------|----------------------------------------|
| `-32700`  | Parse error                            |
| `-32600`  | Invalid request (e.g. wrong jsonrpc)   |
| `-32601`  | Method not found                       |
| `-32602`  | Invalid params                         |
| `-32603`  | Internal error                         |
| `-32001`  | No project open                        |
| `-32002`  | Out of bounds                          |
| `-32003`  | Invalid argument                       |
| `-32004`  | Busy                                   |
| `-32005`  | Failed                                 |
| `-32006`  | Unsupported                            |
| `-32010`  | Permission denied (auth)               |

## Methods

### `agent.hello`

Should be the first call after connecting.

```json
{"jsonrpc":"2.0","id":1,"method":"agent.hello"}
```

```json
{"jsonrpc":"2.0","id":1,"result":{
  "shotcutVersion":"26.4.0",
  "mltVersion":"7.36.0",
  "apiVersion":"1.0.0",
  "capabilities":["project","timeline","clip","filter","player","export","snapshot","undo"]
}}
```

### `agent.subscribe`

Opt in to event topics. The default subscription set is empty.

```json
{"id":2,"method":"agent.subscribe","params":{"topics":["timeline","player","project"]}}
```

Known topics: `timeline`, `player`, `filters`, `project`, `log`.

### `agent.ping`

```json
{"id":3,"method":"agent.ping"} → {"pong":<unix_ms>}
```

### `agent.undo` / `agent.redo`

Reverses or replays the most recent operation on the global undo stack —
the same stack the user's edits live on.

### `agent.dryRun` *(deferred to v1.1)*

Returns `-32006 Unsupported` in this version.

### `project.state`

Returns:

```json
{
  "file": "/path/to/project.mlt",
  "dirty": false,
  "profile": {"width":1920,"height":1080,"fps":30,"sar":1,"colorspace":709,"progressive":true},
  "duration": 1234,
  "resource": "...",
  "undo": {"canUndo":true,"canRedo":false,"count":3,"index":3}
}
```

### `project.open`

```json
{"method":"project.open","params":{"path":"/abs/path/to/file.mlt"}}
{"method":"project.open","params":{"path":"…","discardChanges":true}}
```

Runs synchronously on the GUI thread. Returns `{"ok":true,"file":"<currentFileName>"}`
once the load completes. If the project is dirty and `discardChanges`
(alias `force`) is **not** set, MainWindow puts up a modal "Save unsaved
changes?" dialog and the call blocks until the user answers — so for
unattended scripts pass `discardChanges:true`.

### `project.discardChanges`

```json
{"method":"project.discardChanges"}
```

Drops the dirty flag and clears the undo stack without saving. After this,
a subsequent `project.open` or `project.new` will not surface the modal
"Save changes?" dialog.

### `project.save`

```json
{"method":"project.save","params":{"path":"<optional override>"}}
```

If `path` is omitted the current file is overwritten (matches **File →
Save**). If a `path` is given, behaves like Save As: writes the XML,
updates the current-file pointer, clears the dirty flag, and clears the
undo stack. Returns `{"ok":true,"file":"<path>"}`. Returns `-32005 Failed`
if the write itself fails.

### `project.new`, `project.close`

Trigger the same actions as the menu items; both queue onto the GUI thread
because they may need to prompt the user. Pair with `project.discardChanges`
beforehand if you want to skip the prompt.

### `project.getMltXml`

Returns the entire current MLT XML document. This is the most useful single
method for an agent — give the model the full project state.

```json
{"method":"project.getMltXml"} → {"xml":"<mlt>…</mlt>","length":12345}
```

### `timeline.tracks`

```json
[{"index":0,"name":"V1","type":"video","isMute":false,"isHidden":false,"isLocked":false,"clipCount":3}]
```

### `timeline.clips`

```json
{"method":"timeline.clips","params":{"trackIndex":0,"includeMltXml":false}}
```

Returns an array of `{clipIndex, in, out, start, duration, isBlank, name, resource, hash, mltXml?}`.

### `timeline.selection`

```json
{"clips":[{"track":0,"clip":2}],"currentTrack":0,"isMultitrackSelected":false}
```

### `timeline.markers`

Returns `[{text, start, end, color}]`.

### `timeline.appendClip` / `insertClip` / `overwriteClip`

```json
{"method":"timeline.appendClip","params":{"trackIndex":0,"path":"/some/file.mp4"}}
{"method":"timeline.appendClip","params":{"trackIndex":0,"mltXml":"<producer …/>"}}
```

`mltXml` takes priority. With `path`, the agent server constructs an
avformat producer at the project profile and serializes it for you. `in`
and `out` are optional clamps.

`insertClip` and `overwriteClip` additionally take `position` (in frames;
defaults to current playhead).

### `timeline.removeClip` / `liftClip`

```json
{"params":{"trackIndex":0,"clipIndex":2}}
```

### `timeline.splitClip`

```json
{"params":{"trackIndex":0,"position":<frame>}}
```

Splits the clip under the supplied position (or playhead). Returns
`-32002` if there is no clip there.

### `timeline.moveClip`

```json
{"params":{"fromTrack":0,"fromClip":2,"toTrack":1,"toPosition":300}}
```

Pushes a `MoveClipCommand` to the undo stack with computed `trackDelta`
and `positionDelta`. Note: this does **not** create a transition when
the moved clip overlaps its predecessor — it inserts a blank gap and
trims the predecessor. Use `timeline.addTransition` for cross-fades.

### `timeline.addTransition`

```json
{"params":{"trackIndex":0,"clipIndex":2,"overlapFrames":25,"ripple":false}}
```

Creates a Shotcut dissolve (luma video + cross-fade audio) between
`clip[clipIndex - 1]` and `clip[clipIndex]` on the track. The overlap
is in frames at the project profile fps. Both clips must be real
(non-blank); the predecessor is shortened by `overlapFrames` to make
room for the mix.

Returns `{"ok":true,"transitionClipIndex":<int>,"trackIndex":<int>}`.
The transition shows up in `timeline.clips` as an entry with the name
`"<tractor>"` between the trimmed predecessor and successor.

Backed by `Timeline::AddTransitionCommand` — fully undoable via
`agent.undo`. Returns `-32003` if the boundary isn't a valid spot for
a transition (usually means a neighbour is blank or `overlapFrames`
exceeds the available material).

### `timeline.trimClipIn` / `timeline.trimClipOut`

```json
{"params":{"track":0,"clip":2,"delta":-15}}
```

### `timeline.addTrack` / `removeTrack` / `setTrackProperty`

* `type`: `"video"` or `"audio"`
* Track properties: `name`, `mute` (toggle), `hidden` (toggle),
  `locked` (boolean), `composite` (boolean).

### `timeline.select`

Single clip:

```json
{"params":{"track":0,"clip":2}}
```

Multiple clips:

```json
{"params":{"ranges":[{"track":0,"clip":2},{"track":0,"clip":3}]}}
```

### Filter methods

* `filter.list { target: "output"|"track"|"clip", track?, clip? }` — full
  enumeration of attached filters with their MLT properties.
* `filter.metadata { service }` — schema for a filter service.
* `filter.add`, `filter.remove`, `filter.setParam`, `filter.setKeyframe` —
  **deferred to v1.1**. They return `-32006 Unsupported`. The undo-safe
  add/remove path requires a transient `AttachedFiltersModel` whose
  lifetime survives undo, which is more invasive than the rest of v1.
  Until then, configure filters via the GUI; the agent can read them.

### Player methods

* `player.state` → `{position, isPlaying, in, out, fps, hasProducer}`.
* `player.play { speed? }`, `player.pause`.
* `player.seek { position }` — `position` may be either an integer frame
  count or a timecode string (`"HH:MM:SS.mmm"`, etc.).
* `player.setIn`, `player.setOut` — same position parsing.
* `player.snapshot { format?, position?, width? }` →
  `{format, width, height, position, data: <base64 PNG/JPEG>}`. The bytes
  are the full image — no chunking. At 1080p PNG this is typically 1–4 MB
  per frame; agents on remote links should pass `width` to downscale.

### Export methods

* `export.presets` → list of `consumer/avformat/...` preset names.
* `export.start { path }` — starts an encode using the EncodeDock's
  **currently-loaded preset**. The agent does not pick the preset for v1;
  configure the dock first via the UI. Returns `{ok:true, jobId:N}`. The
  encode runs as an out-of-process `melt` subprocess (via the same
  `JobQueue` the GUI uses), so the resulting file is properly finalized
  on completion. Errors with `-32004 Busy` if another export is in
  progress, the timeline is empty, or the target write isn't permitted.
* `export.jobStatus { jobId }` → `{label, ran, paused, stopped, isFinished, target}`.

### `agent.listMethods` *(diagnostic)*

Returns the registered method names. Convenient for confirming a
mismatched build/spec.

## Server → client notifications

When subscribed:

| Topic       | Notification                | Payload                                    |
|-------------|-----------------------------|--------------------------------------------|
| `timeline`  | `timeline.changed`          | `{revision: N}` (coalesced ~30 Hz)         |
| `timeline`  | `selection.changed`         | `{}`                                       |
| `player`    | `player.position`           | `{position}` (coalesced ~30 Hz)            |
| `player`    | `player.stateChanged`       | `{playing, position?, speed?}`             |
| `project`   | `project.opened`            | `{withReopen}`                             |
| `project`   | `project.saved`             | `{path}`                                   |
| `project`   | `project.closing`           | `{}`                                       |
| `filters`   | `filter.changed`            | `{}`                                       |
| `log`       | `log`                       | `{level, message}` *(deferred to v1.1)*    |

Notifications are dropped silently if the per-session outgoing buffer
exceeds 4 MiB — request responses are never dropped.

## Demo client

```bash
pip install websockets
python scripts/agent_client_demo.py /path/to/project.mlt
```

The script connects, subscribes to timeline and player events, opens the
project, prints frame snapshots, appends a colour-bars clip to V1, and
undoes it. See `scripts/agent_client_demo.py`.

## Known v1 deviations from the original spec

These were chosen explicitly to keep the patch small and the foundation
robust:

* **Single client only.** A second connection is rejected with
  `1008 policy violated`. Multi-client coordination would need a leasing
  protocol that v1 deliberately does not provide.
* **Filter mutations (`add`/`remove`/`setParam`/`setKeyframe`) deferred.**
  Because Shotcut's `Filter::AddCommand` retains a reference to an
  `AttachedFiltersModel`, integrating a transient model whose lifetime
  outlives undo is non-trivial. Reads are fully implemented.
* **`agent.dryRun` deferred.** Most commands have no good "describe
  without committing" surface today.
* **Export uses the EncodeDock's current preset.** The agent supplies
  only the target path. Picking presets via JSON would require either
  rebuilding the entire EncodeDock state-from-preset code as a public
  API or duplicating it; both were larger than v1 warranted.
* **No binary frame transport.** Snapshots come back as base64 PNG/JPEG
  inside text frames. Adequate for localhost; agents over slow links
  should ask for downscaled snapshots.
* **No formal C++ unit tests yet.** Shotcut has no `tests/` directory; the
  Python demo serves as an end-to-end smoke test in v1.

## Threading note

The WebSocket server, all sessions, the dispatcher and every method
handler run on the GUI thread. This is a hard invariant: nothing in the
agent module ever touches `MLT.*`, the multitrack model, or the undo
stack from a worker thread. Any future addition that wants background
work must use `QMetaObject::invokeMethod(target, ..., Qt::QueuedConnection)`
or `QTimer::singleShot(0, ...)` to return to the GUI thread before
mutating state.
