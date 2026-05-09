"""Synchronous Python client for the Shotcut agent server.

A thin, ergonomic wrapper around the JSON-RPC over WebSocket interface
described in docs/agent-api.md. Designed for agent scripts that just
want to drive Shotcut without thinking about asyncio, framing, auth,
or correlation IDs.

Usage:

    from shotcut_agent import Shotcut

    with Shotcut() as sc:
        sc.subscribe(["timeline", "player"])
        sc.project.open("/abs/path.mlt")
        sc.timeline.append_clip(track=0, path="/abs/clip.mp4")
        sc.undo()

Connection defaults:
    url   ws://127.0.0.1:5555/   (override with SHOTCUT_AGENT_URL)
    token $SHOTCUT_AGENT_TOKEN if set, else none

Requires `websockets>=12` (for the sync API). Install with:
    pip install 'websockets>=12'

This module is dependency-light on purpose — it calls into one external
package and the standard library, nothing else.
"""

from __future__ import annotations

import base64
import itertools
import json
import os
import time
from typing import Any, Iterator, Optional

try:
    from websockets.sync.client import connect as _ws_connect
except ImportError as e:
    raise ImportError(
        "shotcut_agent requires websockets>=12. Install: pip install 'websockets>=12'"
    ) from e


__all__ = ["Shotcut", "ShotcutError"]


class ShotcutError(RuntimeError):
    """Raised when the server returns a JSON-RPC error response."""

    def __init__(self, code: int, message: str, data: Any = None):
        super().__init__(f"[{code}] {message}")
        self.code = code
        self.message = message
        self.data = data


# JSON-RPC error code constants — re-exported for convenience.
NO_PROJECT_OPEN = -32001
OUT_OF_BOUNDS = -32002
INVALID_ARGUMENT = -32003
BUSY = -32004
FAILED = -32005
UNSUPPORTED = -32006
PERMISSION_DENIED = -32010


class Shotcut:
    """Sync client for the Shotcut agent server.

    Use as a context manager so the connection always closes cleanly:

        with Shotcut() as sc:
            ...

    The pending event queue is preserved across calls so subscriptions
    deliver reliably even when intermixed with requests.
    """

    def __init__(
        self,
        url: Optional[str] = None,
        token: Optional[str] = None,
        *,
        max_size: int = 32 * 1024 * 1024,
        open_timeout: float = 5.0,
    ) -> None:
        self.url = url or os.environ.get("SHOTCUT_AGENT_URL", "ws://127.0.0.1:5555/")
        self.token = token if token is not None else os.environ.get("SHOTCUT_AGENT_TOKEN", "")
        self._max_size = max_size
        self._open_timeout = open_timeout
        self._ws = None
        self._ids = itertools.count(1)
        self._pending_events: list[dict] = []

        # Namespace bundles. Constructed once per instance so the user
        # writes `sc.timeline.append_clip(...)` instead of digging through
        # method strings.
        self.project = _ProjectNS(self)
        self.timeline = _TimelineNS(self)
        self.player = _PlayerNS(self)
        self.filter = _FilterNS(self)
        self.export = _ExportNS(self)

    # ------------------------------------------------------------- lifecycle

    def __enter__(self) -> "Shotcut":
        self.connect()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.close()

    def connect(self) -> None:
        if self._ws is not None:
            return
        headers = {}
        if self.token:
            headers["Authorization"] = f"Bearer {self.token}"
        self._ws = _ws_connect(
            self.url,
            additional_headers=headers or None,
            max_size=self._max_size,
            open_timeout=self._open_timeout,
        )

    def close(self) -> None:
        if self._ws is not None:
            try:
                self._ws.close()
            finally:
                self._ws = None

    # ------------------------------------------------------------- core RPC

    def call(self, method: str, params: Optional[dict] = None) -> Any:
        """Send a JSON-RPC request and return the result.

        Raises ShotcutError on a server error response.
        Server-pushed notifications received while waiting are queued and
        can be retrieved via `events()`.
        """
        if self._ws is None:
            raise RuntimeError("not connected; call connect() or use as a context manager")
        rid = next(self._ids)
        msg: dict[str, Any] = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        if self.token:
            msg["auth"] = self.token
        self._ws.send(json.dumps(msg))

        # Loop until we see the matching response. Stash any notifications
        # we encounter along the way.
        while True:
            raw = self._ws.recv()
            env = json.loads(raw)
            if env.get("id") == rid:
                if "error" in env:
                    err = env["error"]
                    raise ShotcutError(err.get("code", 0), err.get("message", ""), err.get("data"))
                return env.get("result")
            if env.get("method") and "id" not in env:
                self._pending_events.append(env)
            # Anything else (id mismatch responses) is dropped — there
            # shouldn't be any with our sequential id allocation.

    def notify(self, method: str, params: Optional[dict] = None) -> None:
        """Send a JSON-RPC notification (no id, no response)."""
        if self._ws is None:
            raise RuntimeError("not connected")
        msg: dict[str, Any] = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if self.token:
            msg["auth"] = self.token
        self._ws.send(json.dumps(msg))

    # ----------------------------------------------------------- top-level

    def hello(self) -> dict:
        """First call after connecting. Returns version + capabilities."""
        return self.call("agent.hello")

    def ping(self) -> int:
        """Returns the server's millisecond timestamp."""
        return self.call("agent.ping")["pong"]

    def subscribe(self, topics: list[str]) -> None:
        """Opt in to event topics. Known: timeline, player, filters, project, log."""
        self.call("agent.subscribe", {"topics": list(topics)})

    def undo(self) -> None:
        self.call("agent.undo")

    def redo(self) -> None:
        self.call("agent.redo")

    def list_methods(self) -> list[str]:
        return self.call("agent.listMethods")

    # ------------------------------------------------------------- events

    def events(self, timeout: float = 1.0) -> Iterator[dict]:
        """Yield server-pushed notifications until `timeout` seconds elapse
        with no new event.

        Each yielded value is the full envelope:
            {"jsonrpc": "2.0", "method": "...", "params": {...}}

        Already-queued events from previous calls are flushed first.
        """
        if self._ws is None:
            raise RuntimeError("not connected")
        # Flush queued events from prior `call()` invocations.
        while self._pending_events:
            yield self._pending_events.pop(0)

        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                raw = self._ws.recv(timeout=remaining)
            except TimeoutError:
                return
            env = json.loads(raw)
            if env.get("method") and "id" not in env:
                yield env
                # Reset deadline so we keep yielding while events flow.
                deadline = time.monotonic() + timeout
            elif "id" in env:
                # Stray response — shouldn't happen but be defensive.
                self._pending_events.append(env)


# ===================================================================== namespaces


class _Namespace:
    def __init__(self, sc: Shotcut):
        self._sc = sc


class _ProjectNS(_Namespace):
    def state(self) -> dict:
        return self._sc.call("project.state")

    def open(self, path: str) -> dict:
        return self._sc.call("project.open", {"path": path})

    def save(self, path: Optional[str] = None) -> dict:
        params = {"path": path} if path else None
        return self._sc.call("project.save", params)

    def close(self) -> dict:
        return self._sc.call("project.close")

    def new(self) -> dict:
        return self._sc.call("project.new")

    def get_mlt_xml(self) -> str:
        return self._sc.call("project.getMltXml")["xml"]


class _TimelineNS(_Namespace):
    def tracks(self) -> list[dict]:
        return self._sc.call("timeline.tracks")

    def clips(self, track_index: int, *, include_mlt_xml: bool = False) -> list[dict]:
        return self._sc.call(
            "timeline.clips",
            {"trackIndex": track_index, "includeMltXml": include_mlt_xml},
        )

    def selection(self) -> dict:
        return self._sc.call("timeline.selection")

    def markers(self) -> list[dict]:
        return self._sc.call("timeline.markers")

    def append_clip(
        self,
        track: int,
        *,
        path: Optional[str] = None,
        mlt_xml: Optional[str] = None,
        in_: Optional[int] = None,
        out: Optional[int] = None,
    ) -> dict:
        params: dict[str, Any] = {"trackIndex": track}
        if mlt_xml is not None:
            params["mltXml"] = mlt_xml
        elif path is not None:
            params["path"] = path
        else:
            raise ValueError("append_clip requires either path= or mlt_xml=")
        if in_ is not None:
            params["in"] = in_
        if out is not None:
            params["out"] = out
        return self._sc.call("timeline.appendClip", params)

    def insert_clip(
        self,
        track: int,
        position: int,
        *,
        path: Optional[str] = None,
        mlt_xml: Optional[str] = None,
    ) -> dict:
        params: dict[str, Any] = {"trackIndex": track, "position": position}
        if mlt_xml is not None:
            params["mltXml"] = mlt_xml
        elif path is not None:
            params["path"] = path
        else:
            raise ValueError("insert_clip requires either path= or mlt_xml=")
        return self._sc.call("timeline.insertClip", params)

    def overwrite_clip(
        self,
        track: int,
        position: int,
        *,
        path: Optional[str] = None,
        mlt_xml: Optional[str] = None,
    ) -> dict:
        params: dict[str, Any] = {"trackIndex": track, "position": position}
        if mlt_xml is not None:
            params["mltXml"] = mlt_xml
        elif path is not None:
            params["path"] = path
        else:
            raise ValueError("overwrite_clip requires either path= or mlt_xml=")
        return self._sc.call("timeline.overwriteClip", params)

    def remove_clip(self, track: int, clip: int) -> dict:
        return self._sc.call("timeline.removeClip", {"trackIndex": track, "clipIndex": clip})

    def lift_clip(self, track: int, clip: int) -> dict:
        return self._sc.call("timeline.liftClip", {"trackIndex": track, "clipIndex": clip})

    def split_clip(self, track: int, position: int) -> dict:
        return self._sc.call("timeline.splitClip", {"trackIndex": track, "position": position})

    def move_clip(self, from_track: int, from_clip: int, to_track: int, to_position: int) -> dict:
        return self._sc.call(
            "timeline.moveClip",
            {
                "fromTrack": from_track,
                "fromClip": from_clip,
                "toTrack": to_track,
                "toPosition": to_position,
            },
        )

    def trim_clip_in(self, track: int, clip: int, delta: int) -> dict:
        return self._sc.call(
            "timeline.trimClipIn",
            {"track": track, "clip": clip, "delta": delta},
        )

    def trim_clip_out(self, track: int, clip: int, delta: int) -> dict:
        return self._sc.call(
            "timeline.trimClipOut",
            {"track": track, "clip": clip, "delta": delta},
        )

    def add_track(self, type_: str = "video") -> dict:
        return self._sc.call("timeline.addTrack", {"type": type_})

    def remove_track(self, track: int) -> dict:
        return self._sc.call("timeline.removeTrack", {"trackIndex": track})

    def set_track_property(self, track: int, property_: str, value: Any) -> dict:
        return self._sc.call(
            "timeline.setTrackProperty",
            {"trackIndex": track, "property": property_, "value": value},
        )

    def select(self, *, track: Optional[int] = None, clip: Optional[int] = None,
               ranges: Optional[list[dict]] = None) -> dict:
        if ranges is not None:
            return self._sc.call("timeline.select", {"ranges": ranges})
        if track is None or clip is None:
            raise ValueError("select() needs ranges= or both track= and clip=")
        return self._sc.call("timeline.select", {"track": track, "clip": clip})


class _PlayerNS(_Namespace):
    def state(self) -> dict:
        return self._sc.call("player.state")

    def play(self, speed: float = 1.0) -> dict:
        return self._sc.call("player.play", {"speed": speed})

    def pause(self) -> dict:
        return self._sc.call("player.pause")

    def seek(self, position: int | str) -> dict:
        return self._sc.call("player.seek", {"position": position})

    def set_in(self, position: int | str) -> dict:
        return self._sc.call("player.setIn", {"position": position})

    def set_out(self, position: int | str) -> dict:
        return self._sc.call("player.setOut", {"position": position})

    def snapshot(
        self,
        *,
        format: str = "png",
        position: Optional[int | str] = None,
        width: Optional[int] = None,
    ) -> tuple[bytes, dict]:
        """Return (image_bytes, info). `image_bytes` is already base64-decoded."""
        params: dict[str, Any] = {"format": format}
        if position is not None:
            params["position"] = position
        if width is not None:
            params["width"] = width
        result = self._sc.call("player.snapshot", params)
        data = base64.b64decode(result.pop("data"))
        return data, result

    def snapshot_to(
        self,
        path: str,
        *,
        format: Optional[str] = None,
        position: Optional[int | str] = None,
        width: Optional[int] = None,
    ) -> dict:
        """Save a frame to disk. Format is inferred from path extension if omitted."""
        if format is None:
            ext = path.rsplit(".", 1)[-1].lower() if "." in path else "png"
            format = "jpeg" if ext in ("jpg", "jpeg") else "png"
        data, info = self.snapshot(format=format, position=position, width=width)
        with open(path, "wb") as f:
            f.write(data)
        info["savedTo"] = path
        return info


class _FilterNS(_Namespace):
    def list(self, *, target: str = "output", track: Optional[int] = None,
             clip: Optional[int] = None) -> list[dict]:
        params: dict[str, Any] = {"target": target}
        if track is not None:
            params["track"] = track
        if clip is not None:
            params["clip"] = clip
        return self._sc.call("filter.list", params)

    def metadata(self, service: str) -> dict:
        return self._sc.call("filter.metadata", {"service": service})


class _ExportNS(_Namespace):
    def presets(self) -> list[str]:
        return self._sc.call("export.presets")

    def start(self, path: str) -> dict:
        return self._sc.call("export.start", {"path": path})

    def job_status(self, job_id: int) -> dict:
        return self._sc.call("export.jobStatus", {"jobId": job_id})

    def wait(self, job_id: int, *, poll: float = 1.0, timeout: Optional[float] = None) -> dict:
        """Block until the job is finished, polling every `poll` seconds."""
        deadline = time.monotonic() + timeout if timeout else None
        while True:
            status = self.job_status(job_id)
            if status.get("isFinished"):
                return status
            if deadline is not None and time.monotonic() > deadline:
                raise TimeoutError(f"job {job_id} did not finish within {timeout}s")
            time.sleep(poll)
