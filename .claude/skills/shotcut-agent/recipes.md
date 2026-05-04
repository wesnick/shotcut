# Recipes

Short snippets the agent can adapt. All assume the import boilerplate:

```python
import sys
sys.path.insert(0, ".claude/skills/shotcut-agent")
from shotcut_agent import Shotcut, ShotcutError, NO_PROJECT_OPEN, UNSUPPORTED
```

## Inspect the project deeply

```python
with Shotcut() as sc:
    state = sc.project.state()
    xml = sc.project.get_mlt_xml()  # full MLT XML — feed to a model

    print(f"file={state.get('file')!r} dirty={state['dirty']}")
    print(f"profile: {state['profile']}")
    print(f"tracks: {len(sc.timeline.tracks())}")
    print(f"xml length: {len(xml)} chars")
```

## Append a clip and verify visually

```python
with Shotcut() as sc:
    sc.timeline.append_clip(track=0, path="/abs/path/clip.mp4")

    # Find where it landed: last clip on track 0
    clips = sc.timeline.clips(0)
    last = clips[-1]
    sc.player.seek(last["start"] + last["duration"] // 2)
    sc.player.snapshot_to("/tmp/midpoint.jpg", width=480)
```

## Trim the head of every clip on V1 by N frames

```python
with Shotcut() as sc:
    clips = sc.timeline.clips(0)
    for c in reversed(clips):  # iterate back-to-front so indices stay valid
        if c["isBlank"]:
            continue
        sc.timeline.trim_clip_in(track=0, clip=c["clipIndex"], delta=10)
```

## Watch the playhead live

```python
with Shotcut() as sc:
    sc.subscribe(["player"])
    sc.player.play()
    for ev in sc.events(timeout=10.0):
        if ev["method"] == "player.position":
            print(ev["params"]["position"])
        elif ev["method"] == "player.stateChanged" and not ev["params"]["playing"]:
            break
```

## React to user-driven timeline edits

```python
with Shotcut() as sc:
    sc.subscribe(["timeline"])
    last_rev = 0
    for ev in sc.events(timeout=30.0):
        if ev["method"] == "timeline.changed":
            rev = ev["params"]["revision"]
            if rev != last_rev:
                last_rev = rev
                # Re-fetch state and react.
                tracks = sc.timeline.tracks()
                print(f"rev {rev}: {sum(t['clipCount'] for t in tracks)} clips total")
```

## Defensive patterns

```python
with Shotcut() as sc:
    try:
        xml = sc.project.get_mlt_xml()
    except ShotcutError as e:
        if e.code == NO_PROJECT_OPEN:
            print("no project; ask the user to open one")
        else:
            raise

    try:
        sc.filter.list(target="clip", track=0, clip=0)
    except ShotcutError as e:
        if e.code == UNSUPPORTED:
            print("v1 filter mutations are not implemented; reads are")
```

## Color-bars test clip

A handy MLT producer for sanity-checking timeline writes without
needing a media file on disk:

```python
COLOR_BARS_XML = (
    '<mlt><producer id="bars" mlt_service="color" '
    'resource="0xff0000ff" in="0" out="49"/></mlt>'
)

with Shotcut() as sc:
    sc.timeline.append_clip(track=0, mlt_xml=COLOR_BARS_XML)
    sc.undo()
```

## Driving export

The export RPC uses the dock's currently-loaded preset. If the user
hasn't picked one yet, the encode will use whatever defaults are
showing. Walk them through it first:

```python
with Shotcut() as sc:
    presets = sc.export.presets()
    print("presets available:", presets[:10])
    print("Configure your preset in Export, then call:")
    print('   sc.export.start("/tmp/out.mp4")')
```

Once the user has the preset configured:

```python
with Shotcut() as sc:
    job = sc.export.start("/tmp/out.mp4")
    final = sc.export.wait(job["jobId"], poll=2.0, timeout=600)
    print(final)
```
