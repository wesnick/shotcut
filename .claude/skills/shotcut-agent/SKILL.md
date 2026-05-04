---
name: shotcut-agent
description: Drive a running Shotcut video editor over its JSON-RPC WebSocket agent server — query timeline state, run edits, capture frame snapshots, subscribe to live events. Use when the user asks to control Shotcut programmatically, edit a project from a script, inspect what's currently loaded, or drive Shotcut from another agent. Prefer this over manually hand-rolling websocket calls.
---

# Shotcut agent

This skill talks to the **Shotcut Agent Server** — a JSON-RPC over WebSocket
endpoint Shotcut exposes when the user enables **Settings → AI Agent…** or
launches with `--agent-server`.

## Working style: write Python, don't ask for tools

You drive Shotcut by **writing Python that uses
`scripts/shotcut_agent.py`** (the small client library shipped in this
skill). Run it via `python -c '…'` or save to a file and execute. **Do not
ask for new tools to be wired up per RPC method** — the library already
covers everything the server exposes plus convenience helpers.

## Quick start

```python
import sys
sys.path.insert(0, ".claude/skills/shotcut-agent")
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
* relaunch Shotcut with `shotcut --agent-server` (`=PORT` to override).

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

## Filter mutations are read-only in v1

`sc.filter.list(...)` and `sc.filter.metadata(service)` work. Adding,
removing, or parameter-editing filters returns `-32006 Unsupported`
(see `docs/agent-api.md` for why). If you need to apply a filter, ask
the user to do it in the UI; you can read it back after.

## Export

`sc.export.start(path)` triggers an encode using the **EncodeDock's
currently-loaded preset** — it does **not** select the preset for you.
Have the user configure the preset in the dock first, then call
`export.start` with just the target path. Poll `sc.export.job_status(job_id)`
until `is_finished` is true.

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
