# Recipes

Short snippets the agent can adapt. All assume the import boilerplate:

```python
import sys
sys.path.insert(0, ".claude/skills/shotcut-agent/scripts")
from shotcut_agent import Shotcut, ShotcutError, NO_PROJECT_OPEN, UNSUPPORTED
```

(Or skip the boilerplate entirely by running these snippets via
`uv run .claude/skills/shotcut-agent/scripts/shotcut_agent.py`, which
pre-binds `Shotcut` and an open `sc` for you.)

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

## Add cross-fade transitions between adjacent clips

```python
with Shotcut() as sc:
    fps = sc.project.state()["profile"]["fps"]
    overlap = int(round(1.0 * fps))  # 1-second dissolve

    # Walk back-to-front because each addTransition re-indexes the
    # boundary as a tractor entry, shifting later clips down by one.
    boundaries = []
    clips = sc.timeline.clips(0)
    for i in range(1, len(clips)):
        prev, cur = clips[i - 1], clips[i]
        if not prev["isBlank"] and not cur["isBlank"]:
            boundaries.append(i)

    for clip_index in reversed(boundaries):
        sc.timeline.add_transition(track=0, clip=clip_index, overlap=overlap)
```

## Reset to a clean project, even if there are unsaved edits

```python
with Shotcut() as sc:
    # discard_changes=True dismisses the "Save changes?" modal that
    # would otherwise block project.open silently.
    sc.project.open(
        ".claude/skills/shotcut-agent/assets/empty.mlt",
        discard_changes=True,
    )
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

## Speed up silent gaps with a text indicator

Real-world use case: a screencast where the narrator went silent
while demonstrating a long-running process. Compress those silent
stretches to 10× speed and overlay a label so the viewer sees both
the speed-up *and* the underlying work being performed.

Given a transcript with per-segment `{start, end}` timestamps (e.g.
from Whisper / Voxtral), the strategy is:

1. Compute silent gaps from the segment boundaries.
2. Merge gaps shorter than a threshold (e.g. 5s — sub-5s warps are
   jarring) into the adjacent speech run.
3. Emit one `.mlt` with **two shared producers**: an `avformat` for
   the source at 1×, and a `timewarp` for the source at 10×. The
   `timewarp` carries a `dynamictext` filter with `#timecode#` so it
   shows a ticking counter even on a static-looking screen.
4. The playlist alternates `<entry producer="src">` slices for speech
   and `<entry producer="warp">` slices for silence. Each warp entry
   uses **warped** time coordinates (`source_t / speed`).
5. Open with `sc.project.open(out, discard_changes=True)`.

```python
# /// script
# dependencies = []
# ///
import json
from pathlib import Path
from xml.sax.saxutils import escape

SRC = "/abs/path/source.mkv"
TRANSCRIPT = "/abs/path/transcript.json"   # has "segments": [{start, end}, …]
SOURCE_DURATION = 1641.5  # seconds; ffprobe -show_entries format=duration
SPEED = 10.0
MIN_GAP = 5.0           # seconds; sub-5s gaps absorbed into speech
FPS = 25
OUT = "/abs/path/auto_skip.mlt"


def hms(s: float) -> str:
    ms = round(max(s, 0) * 1000)
    h, rem = divmod(ms, 3600 * 1000)
    m, rem = divmod(rem, 60 * 1000)
    sec, ms = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{sec:02d}.{ms:03d}"


def plan(segments, src_duration, min_gap):
    """Return [('speech'|'warp', start, end), …] covering [0, src_duration]."""
    segs = sorted(((s["start"], s["end"]) for s in segments))
    runs: list[list[float]] = []
    for st, en in segs:
        if runs and st <= runs[-1][1] + min_gap:
            runs[-1][1] = max(runs[-1][1], en)
        else:
            runs.append([st, en])
    clips, cursor = [], 0.0
    for st, en in runs:
        if st - cursor >= min_gap:
            clips.append(("warp", cursor, st))
            clips.append(("speech", st, en))
        else:
            clips.append(("speech", cursor if st - cursor > 0 else st, en))
        cursor = en
    if src_duration - cursor >= min_gap:
        clips.append(("warp", cursor, src_duration))
    elif clips and clips[-1][0] == "speech":
        k, s, _ = clips[-1]
        clips[-1] = (k, s, src_duration)
    return clips


def build(clips, src_video, src_duration, speed, fps):
    src = str(Path(src_video).resolve())
    src_xml = escape(src)
    # CRITICAL: speed-prefix form. NOT "timewarp:<speed>:<path>".
    warp_resource = escape(f"{speed}:{src}")
    warp_duration = src_duration / speed
    label = f"{int(speed)}x  >>  #timecode#"
    entries = []
    for kind, st, en in clips:
        if kind == "speech":
            entries.append(
                f'    <entry producer="src" in="{hms(st)}" '
                f'out="{hms(en - 1.0/fps)}"/>')
        else:
            entries.append(
                f'    <entry producer="warp" in="{hms(st/speed)}" '
                f'out="{hms(en/speed - 1.0/fps)}"/>')
    return f"""<?xml version="1.0" standalone="no"?>
<mlt LC_NUMERIC="C" version="7.39.0" title="auto-skip" producer="main_bin">
  <profile description="HD 1080p 25 fps" width="1920" height="1080"
    progressive="1" sample_aspect_num="1" sample_aspect_den="1"
    display_aspect_num="16" display_aspect_den="9"
    frame_rate_num="25" frame_rate_den="1" colorspace="709"/>
  <playlist id="main_bin"><property name="xml_retain">1</property></playlist>
  <producer id="black" in="00:00:00.000" out="{hms(src_duration)}">
    <property name="length">{hms(src_duration + 1.0/fps)}</property>
    <property name="eof">pause</property>
    <property name="resource">0</property>
    <property name="mlt_service">color</property>
  </producer>
  <playlist id="background">
    <property name="shotcut:projectAudioChannels">2</property>
    <entry producer="black" in="00:00:00.000" out="{hms(src_duration)}"/>
  </playlist>
  <producer id="src" in="00:00:00.000" out="{hms(src_duration - 1.0/fps)}">
    <property name="length">{hms(src_duration)}</property>
    <property name="resource">{src_xml}</property>
    <property name="mlt_service">avformat</property>
    <property name="audio_index">1</property>
    <property name="video_index">0</property>
  </producer>
  <producer id="warp" in="00:00:00.000" out="{hms(warp_duration - 1.0/fps)}">
    <property name="length">{hms(warp_duration)}</property>
    <property name="resource">{warp_resource}</property>
    <property name="warp_speed">{speed}</property>
    <property name="warp_resource">{src_xml}</property>
    <property name="warp_pitch">1</property>
    <property name="mlt_service">timewarp</property>
    <property name="audio_index">1</property>
    <property name="video_index">0</property>
    <filter id="dyn">
      <property name="mlt_service">dynamictext</property>
      <property name="argument">{escape(label)}</property>
      <property name="geometry">0% 88%:100%x10%</property>
      <property name="family">Verdana</property>
      <property name="size">56</property>
      <property name="weight">700</property>
      <property name="fgcolour">#ffffffff</property>
      <property name="bgcolour">#b8000000</property>
      <property name="halign">center</property>
      <property name="valign">middle</property>
      <property name="outline">3</property>
      <property name="opacity">1</property>
    </filter>
  </producer>
  <playlist id="playlist0">
    <property name="shotcut:video">1</property>
    <property name="shotcut:name">V1</property>
{chr(10).join(entries)}
  </playlist>
  <tractor id="tractor0" in="00:00:00.000" out="{hms(src_duration)}">
    <property name="shotcut">1</property>
    <property name="shotcut:projectAudioChannels">2</property>
    <track producer="background"/>
    <track producer="playlist0"/>
    <transition><property name="a_track">0</property>
      <property name="b_track">1</property>
      <property name="mlt_service">mix</property>
      <property name="always_active">1</property>
      <property name="sum">1</property></transition>
  </tractor>
</mlt>"""


data = json.loads(Path(TRANSCRIPT).read_text())
clips = plan(data["segments"], SOURCE_DURATION, MIN_GAP)
Path(OUT).write_text(build(clips, SRC, SOURCE_DURATION, SPEED, FPS))
```

Then load and verify the 10× actually takes effect by checking that
two frames inside the same warp clip show **different** screen
content (if they don't, the resource format is wrong and MLT silently
defaulted speed to 1.0):

```python
import sys, time, subprocess
sys.path.insert(0, ".claude/skills/shotcut-agent/scripts")
from shotcut_agent import Shotcut

with Shotcut() as sc:
    sc.project.open(OUT, discard_changes=True)
    while sc.project.state().get("file") != OUT:
        time.sleep(0.3)
    # first warp clip in the timeline:
    clips = sc.timeline.clips(0)
    warp = next(c for c in clips if "warp" in c["resource"].lower()
                                  or c.get("name", "").lower().startswith("silent"))
    a, b = warp["start"] + 2, warp["start"] + warp["duration"] - 2
    sc.player.seek(a); time.sleep(0.8); sc.player.snapshot_to("/tmp/a.png", width=320)
    sc.player.seek(b); time.sleep(0.8); sc.player.snapshot_to("/tmp/b.png", width=320)
    ha = subprocess.check_output(["sha1sum", "/tmp/a.png"]).split()[0]
    hb = subprocess.check_output(["sha1sum", "/tmp/b.png"]).split()[0]
    assert ha != hb, "warp clip is showing identical frames — speed didn't apply"
```

If `a` and `b` hash identical, double-check the resource string — it
must be `<speed>:<path>`, not `timewarp:<speed>:<path>`. See the
**Custom producers** section of `SKILL.md` for the full explanation
of that bug.

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
