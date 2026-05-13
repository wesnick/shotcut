---
name: shotcut-agent
description: Drive a running Shotcut video editor over its JSON-RPC WebSocket agent server — query timeline state, run edits, capture frame snapshots, subscribe to live events. Use when the user asks to control Shotcut programmatically, edit a project from a script, inspect what's currently loaded, or drive Shotcut from another agent. Prefer this over manually hand-rolling websocket calls.
---

# Shotcut agent

This skill talks to the **Shotcut Agent Server** — a JSON-RPC over WebSocket
endpoint Shotcut exposes when the user enables **Settings → AI Agent…** or
launches with `--agent-server`.

## Skill layout

```
.claude/skills/shotcut-agent/
├── SKILL.md                      (this file)
├── scripts/
│   └── shotcut_agent.py          (library + PEP 723 self-runner)
├── references/
│   └── recipes.md                (longer recipe collection)
└── assets/
    └── empty.mlt                 (minimal Shotcut project fixture)
```

The skill directory is **read-only** for sessions — definition only.
Agent-generated artifacts (test scripts, exported clips, findings,
scratch fixtures) belong in the project workspace, **not** inside
`.claude/skills/shotcut-agent/`. See **Workspace** below.

## Workspace

This repo's session workspace is at the repo root under a stable
quirky-name directory (gitignored via `.git/info/exclude`). Look for an
existing top-level dir matching `<adj>-<noun>-<noun>` (e.g.
`grumpy-quokka-saucer/`); that's the workspace. If none exists, create
one and add it to `.git/info/exclude`. Use it for:

* exported MP4s, snapshot PNGs, generated `.mlt` fixtures,
* throwaway test scripts driving the agent,
* session findings docs, hand-off notes.

Never write working artifacts into `.claude/skills/shotcut-agent/`. The
fixture at `assets/empty.mlt` is read-only — copy it into the workspace
before opening if Shotcut might write staging files alongside it.

## Working style: write Python, don't ask for tools

Drive Shotcut by **writing Python that uses
`scripts/shotcut_agent.py`** (the small client library shipped in this
skill). **Do not ask for new tools to be wired up per RPC method** —
the library already covers everything the server exposes plus
convenience helpers.

## Quick start (PEP 723 — no manual venv)

`scripts/shotcut_agent.py` is both a library and a self-runnable
script. It declares `websockets` inline (PEP 723), so `uv run`
installs the dep into a transient venv on first use — no `pip install`
needed:

```bash
uv run .claude/skills/shotcut-agent/scripts/shotcut_agent.py -c 'print(sc.hello())'

uv run .claude/skills/shotcut-agent/scripts/shotcut_agent.py <<'PY'
print(sc.project.state())
for t in sc.timeline.tracks():
    print(t["name"], len(sc.timeline.clips(t["index"])))
PY
```

In script mode, `Shotcut` and an already-opened `sc` are pre-bound in
the snippet's globals; the connection is closed when the snippet exits.
Pass `--no-connect` to skip the auto-open, or `--url` / `--token` to
override defaults.

For longer scripts, save them as `.py` and add your own PEP 723 header:

```python
# /// script
# dependencies = ["websockets>=12"]
# ///
import sys
sys.path.insert(0, ".claude/skills/shotcut-agent/scripts")
from shotcut_agent import Shotcut

with Shotcut() as sc:                       # ws://127.0.0.1:5555/, $SHOTCUT_AGENT_TOKEN
    print(sc.hello())                       # version / capabilities
    sc.subscribe(["timeline", "player"])
    sc.project.open("/abs/path/to/file.mlt")

    state = sc.project.state()
    print(state["file"], state["profile"]["fps"])

    tracks = sc.timeline.tracks()
    print(f"{len(tracks)} track(s)")

    sc.player.seek(30)
    sc.player.snapshot_to("/tmp/frame.png", width=480)

    sc.timeline.append_clip(track=0, path="/abs/path/to/clip.mp4")
    sc.undo()                               # roll the append back
```

Then run with `uv run path/to/your_script.py`.

The library gives you:

* sync (blocking) client — no asyncio in your scripts
* dotted namespaces: `sc.project.*`, `sc.timeline.*`, `sc.player.*`,
  `sc.filter.*`, `sc.export.*`
* `sc.call(method, params)` for anything the namespaces don't wrap
* `sc.events(timeout=...)` to consume server-pushed notifications
* `sc.player.snapshot_to(path, width=...)` saves base64 directly to disk
* automatic `Authorization: Bearer …` from `SHOTCUT_AGENT_TOKEN` env var

## Connection

Default URL is `ws://127.0.0.1:5555/`. Override with the `SHOTCUT_AGENT_URL`
env var, or pass `Shotcut(url=..., token=...)` explicitly. Single-client
server: only one connection at a time, so close yours before retrying.

If `agent.hello` fails immediately the server probably isn't running.
Tell the user to:

* tick **Settings → AI Agent… → Enable agent server** and restart, **or**
* relaunch Shotcut with `shotcut --agent-server=5555` (the `=PORT` is
  required — bare `--agent-server` errors out with "Missing value").

## You need an open project before timeline ops work

`project.new` triggers Shotcut's "close current project" UI action — it
does **not** create a fresh empty timeline you can manipulate. After
calling it, `timeline.*` methods will return `-32001 no project is
open`. To get a usable timeline:

* call `sc.project.open(path)` on an existing `.mlt`, **or**
* open the bundled empty fixture:
  `.claude/skills/shotcut-agent/assets/empty.mlt` (a minimal valid
  Shotcut project with one V1 track, suitable for test-driving).

### Switching projects when the current one is dirty

`project.open(path)` will pop a modal "Save changes?" dialog if the
current project is dirty. The agent client cannot dismiss the dialog,
so the call appears to hang and `state.file` keeps showing the previous
file. Two ways to avoid this:

```python
# Discard pending edits and load — the file is replaced even if dirty:
sc.project.open("/path/to/other.mlt", discard_changes=True)

# Or do it as an explicit two-step gesture:
sc.project.discard_changes()
sc.project.open("/path/to/other.mlt")
```

Without one of these, plan to **save first** with `sc.project.save(path)`
which now correctly clears the dirty flag and updates the current-file
pointer.

### Autosave-recovery dialog also blocks `project.open` (Issue 7)

`discard_changes=True` bypasses the *Save changes?* modal but **not**
the *Auto-saved files exist. Do you want to recover them now?* modal.
That dialog appears whenever
`~/.local/share/Meltytech/Shotcut/autosave/<md5(abs_path)>.mlt` exists
for the target file, and like the save-changes modal it blocks the
agent's `project.open` call silently — the client just waits, then
times out on the keepalive.

The agent client can't dismiss the dialog. Options:

* Ask the user to click *No* on the dialog, then retry the open.
* Delete the stale autosave hash file before opening:

  ```python
  import hashlib, os
  hash_path = os.path.expanduser(
      "~/.local/share/Meltytech/Shotcut/autosave/"
      + hashlib.md5(b"/abs/path/to/file.mlt").hexdigest() + ".mlt"
  )
  if os.path.exists(hash_path):
      os.remove(hash_path)
  sc.project.open("/abs/path/to/file.mlt", discard_changes=True)
  ```

* Restart Shotcut with the project file as a CLI argument
  (`shotcut --agent-server=5555 /abs/path/to/file.mlt`) — the
  autosave-recovery prompt still fires but it only appears once at
  launch, before any agent client connects.

Fix tracked as Issue 7 in `FINDINGS.md` — when patched, the
`project.open` handler will delete the stale entry itself when
`discard_changes=True`.

### `state.file` is not a reliable "load succeeded" signal

For projects that have never been saved (the bundled `empty.mlt`
fixture is one), `project.state()["file"]` returns `""` even after a
successful open. Use `len(sc.timeline.tracks()) > 0` or read back
`sc.project.get_mlt_xml()` instead.

## Edits go through Shotcut's undo stack

Every mutation (`timeline.appendClip`, `timeline.removeClip`, `clip.move`,
`timeline.addTrack`, …) pushes a `QUndoCommand` onto the same undo stack
the user's GUI edits live on. **Always test with a follow-up `sc.undo()`**
to confirm the user can roll your change back. The `agent.undo` /
`agent.redo` RPCs do exactly what ⌘Z / ⌘⇧Z do in the UI.

## Reading project state

The single most useful read is `sc.project.get_mlt_xml()` — it returns
the entire project as MLT XML. Feed that to a model when you need full
context. For coarse summaries use `sc.project.state()`,
`sc.timeline.tracks()`, and `sc.timeline.clips(track_index=N)`.

## Frame snapshots

`sc.player.snapshot(width=W)` returns `(png_bytes, info_dict)` where
`png_bytes` is the raw decoded image (already base64-decoded). For
quick visual feedback prefer `snapshot_to(path, width=W)`. Stick to
narrow widths (320–640) unless you really need full resolution; full
1080p PNGs are 1–4 MB per frame.

**Snapshots inside transitions are unreliable.** When the playhead
sits on a `lumaMix` transition tractor, snapshots can return heavily
artifacted frames (per-channel garbled). The same project rendered
through `export.start` looks correct. Don't use snapshots to verify
transitions — render a short proof clip instead, or seek to a frame
clearly outside the overlap window.

### Player events only fire during playback

`sc.subscribe(["player"])` followed by programmatic `sc.player.seek(N)`
calls **does not** emit `player.position` notifications — those are
emitted only while transport playback is running (`sc.player.play()`).
If you need to react to seek, poll `sc.player.state()` instead.

## Filter mutations are read-only in v1

`sc.filter.list(...)` and `sc.filter.metadata(service)` work. Adding,
removing, or parameter-editing filters returns `-32006 Unsupported`
(see `docs/agent-api.md` for why). If you need to apply a filter, ask
the user to do it in the UI; **or** ship the filter inside a hand-
rolled producer XML via `timeline.appendClip(mltXml=...)` (see next
section).

## Custom producers (timewarp, with-filter) via mltXml

`timeline.appendClip` / `insertClip` / `overwriteClip` accept a raw
`mltXml=` parameter instead of `path=`. The XML is treated as a self-
contained MLT producer (or chain) and appended to the track exactly
like a UI drag-in. This is the official escape hatch for everything
the typed RPCs don't cover:

* clips at a non-1.0 speed (`mlt_service="timewarp"`),
* clips with filters already attached (`dynamictext`, `affine`, etc.),
* virtual producers (`color`, `noise`, `pango`),
* multi-clip tractors for hand-rolled transitions.

Minimum shape:

```python
warp_xml = f"""<?xml version="1.0" encoding="utf-8"?>
<mlt LC_NUMERIC="C" version="7.39.0">
  <producer id="w" in="00:00:00.000" out="00:00:17.720">
    <property name="length">00:00:17.760</property>
    <property name="resource">10.0:/abs/path/to/source.mkv</property>
    <property name="warp_speed">10.0</property>
    <property name="warp_resource">/abs/path/to/source.mkv</property>
    <property name="warp_pitch">1</property>
    <property name="mlt_service">timewarp</property>
    <filter id="f">
      <property name="mlt_service">dynamictext</property>
      <property name="argument">10x  &gt;&gt;  #timecode#</property>
      <property name="geometry">0% 88%:100%x10%</property>
      <property name="size">56</property>
      <property name="fgcolour">#ffffffff</property>
      <property name="bgcolour">#b8000000</property>
      <property name="halign">center</property>
      <property name="valign">middle</property>
    </filter>
  </producer>
</mlt>"""
sc.timeline.append_clip(track=0, mlt_xml=warp_xml)
```

The library param is `mlt_xml=` (snake_case); the JSON-RPC field is
`mltXml`. Both `appendClip` and `insertClip` accept it.

### ★ Timewarp resource format ★ — silent footgun

MLT's timewarp producer parses speed via `atof()` on the resource
string. **Two formats coexist in the wild and only one works** when
the project is loaded from XML:

| Format                             | Source           | What MLT actually does    |
|------------------------------------|------------------|---------------------------|
| `<speed>:<abs_path>` (e.g. `10.0:/x.mkv`) | What MLT expects | speed = 10.0 ✓            |
| `timewarp:<speed>:<abs_path>`      | What Shotcut's own writer emits via `util.cpp` | atof sees "t" → returns 0 → **speed silently falls back to 1.0** ✗ |

The Shotcut-style form is *visible* in saved projects but it's a
roundtrip bug — `mlt_xml_prefix_size` in `modules/xml/common.c` only
recognises a numeric prefix, and `producer_loader.c::create_producer`
splits at the first colon, so the timewarp factory ends up with
`arg = "timewarp:10.0:/path"` where `atof()` returns 0 → speed
defaults to 1.0. The clip then plays the source at **1× speed**,
clipped to the entry's in/out window — visually indistinguishable
from a "skip" of unrelated source content. Worth pushing a fix to
Shotcut's writer; in the meantime, when generating MLT XML by hand
**always use `<speed>:<path>`**.

Two other gotchas with timewarp clips:

* The `<producer>` length is `source_duration / speed`, not source
  duration. Each `<entry producer="warp" in=A out=B>` specifies in/out
  in the **warped** timeline. So to play source seconds `[s0, s1)` at
  10×, write `in=s0/10, out=s1/10` on the entry.
* The clip's *displayed* `resource` string after load will be
  `<project_dir>/<speed>:<path>` because MLT's XML qualifier prepends
  the project root to anything that doesn't look absolute. This is a
  display artifact — `warp_resource` retains the real path and
  playback is correct.

### Shared producer pattern

When emitting many clips that all reference the same source, write
**one `<producer>` per unique resource** and many `<entry>` items
pointing at slices of it. That's what Shotcut itself does on save —
it keeps the XML small and lets you attach a single filter chain
that fires for every `<entry>`. Useful for, e.g., a single 10× warp
producer that drives a dozen sped-up silent-gap entries with one
shared `dynamictext` overlay.

### dynamictext keywords

`mlt_service="dynamictext"` accepts these substitution tokens inside
the `argument` property (from `modules/plus/filter_dynamictext.c`):

```
#timecode#       SMPTE drop-frame timecode of the current frame
#smpte_ndf#      same, non-drop-frame
#frame#          frame number
#filedate#       producer file mtime (GMT)
#localfiledate#  producer file mtime (local TZ)
#localtime#      wall-clock time now
#createdate#     guessed file creation date
#resource#       producer resource string
```

Any property name is also valid — e.g. `#meta.media.0.codec.frame_rate#`.
A delimiter + strftime spec customises time formatting:
`#localtime %I:%M %p#`. The `#` is escaped with `\`.

`#timecode#` on a timewarp producer ticks at wall-clock framerate
(1s per real second), making it an effective visual cue that
playback is progressing during sped-up static screens — without it,
a 10× clip over a static screen looks like a frozen frame.

See `references/recipes.md` for a full "speed up silent gaps with a
text indicator" recipe that ties this together.

## Transitions

Use `sc.timeline.add_transition(track=N, clip=K, overlap=F)` to create
a Shotcut dissolve between `clip[K-1]` and `clip[K]` on the track. The
overlap is in frames at the project profile fps. Both neighbours must
be real (non-blank) clips; the predecessor is shortened by the overlap
amount on the way in. The transition rides on the same undo stack as
GUI edits — `sc.undo()` removes it.

`timeline.moveClip` (`sc.timeline.move_clip`) does NOT auto-create a
transition when the moved clip overlaps its neighbour — it inserts a
blank gap and trims instead. Use `add_transition` for visible
cross-fades.

## Export

`sc.export.start(path)` triggers an encode using the **EncodeDock's
currently-loaded preset** — it does **not** select the preset for you.
Have the user configure the preset in the dock first, then call
`export.start` with just the target path. The reply has a `jobId`;
poll `sc.export.job_status(job_id)` (or `sc.export.wait(job_id)`) until
`is_finished` is true. The encode runs as an out-of-process `melt`
subprocess, so the resulting MP4 is properly finalized (moov atom
written) when the job completes.

## Events

```python
sc.subscribe(["timeline", "player", "project"])
for ev in sc.events(timeout=5.0):
    if ev["method"] == "player.position":
        print("playhead at", ev["params"]["position"])
    if ev["method"] == "timeline.changed":
        print("timeline rev", ev["params"]["revision"])
```

`events(timeout=...)` yields each server notification until the timeout
expires with no new events. Topics: `timeline`, `player`, `filters`,
`project`, `log`. Unsubscribed topics are dropped server-side.

## Common patterns

**"What's in the project right now?"**
```python
with Shotcut() as sc:
    print(sc.project.get_mlt_xml())
```

**"Add this video to V1, then show me a frame at 5 seconds."**
```python
with Shotcut() as sc:
    fps = sc.project.state()["profile"]["fps"]
    sc.timeline.append_clip(track=0, path="/abs/path.mp4")
    sc.player.seek(round(5 * fps))
    sc.player.snapshot_to("/tmp/check.jpg", format="jpeg", width=640)
```

**"Roll back my last change."**
```python
with Shotcut() as sc:
    sc.undo()
```

**"List all clips on every track."**
```python
with Shotcut() as sc:
    for t in sc.timeline.tracks():
        clips = sc.timeline.clips(t["index"])
        print(t["name"], "→", [c["resource"] for c in clips])
```

## Errors

Library raises `ShotcutError(code, message)` on JSON-RPC error
responses. Codes worth handling specifically:

| Code     | Meaning            |
|----------|--------------------|
| `-32001` | No project open    |
| `-32002` | Out of bounds      |
| `-32003` | Invalid argument   |
| `-32004` | Busy (e.g. export) |
| `-32006` | Unsupported (v1)   |
| `-32010` | Auth required      |

## Reference

Full RPC reference (parameters, return shapes, error codes) is in
`docs/agent-api.md` at the repo root. The library here is a thin sync
wrapper over those methods — anything the server exposes is reachable
via `sc.call("namespace.method", {...})`.
