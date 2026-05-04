#!/usr/bin/env python3
# Copyright (c) 2026 Shotcut contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Demo client for the Shotcut agent JSON-RPC over WebSocket server.
#
# Usage:
#   pip install websockets
#   python scripts/agent_client_demo.py /abs/path/to/project.mlt
#
# Optional flags:
#   --uri ws://host:port/   default ws://127.0.0.1:5555/
#   --token TOKEN           bearer token (only required if the server has one)
#
# What it does:
#   1. agent.hello, prints capabilities
#   2. agent.subscribe(timeline + player + project)
#   3. project.open the supplied path
#   4. player.seek to frame 30, then player.snapshot (saved to /tmp)
#   5. timeline.appendClip with a color: producer XML on track 0
#   6. agent.undo to roll back the appended clip
#
# This is a demo, not a library — error handling is minimal on purpose.

import argparse
import asyncio
import base64
import itertools
import json
import os
import sys

try:
    import websockets
except ImportError:
    sys.stderr.write("error: pip install websockets\n")
    sys.exit(1)


_id_iter = itertools.count(1)


async def call(ws, method, params=None, *, token=None):
    """Send a JSON-RPC request and wait for the matching response."""
    msg = {"jsonrpc": "2.0", "id": next(_id_iter), "method": method}
    if params is not None:
        msg["params"] = params
    if token:
        msg["auth"] = token
    await ws.send(json.dumps(msg))
    while True:
        raw = await ws.recv()
        env = json.loads(raw)
        if env.get("id") == msg["id"]:
            if "error" in env:
                raise RuntimeError(f"{method} → {env['error']}")
            return env.get("result")
        # Notifications and out-of-order responses: keep reading.
        method_in = env.get("method")
        if method_in:
            print(f"<< notification {method_in}: {env.get('params')}")


COLOR_BARS_XML = """<mlt>
  <producer id="bars" mlt_service="color" resource="0xff0000ff" in="0" out="49"/>
</mlt>"""


async def run(uri, project_path, token):
    async with websockets.connect(
        uri,
        additional_headers={"Authorization": f"Bearer {token}"} if token else None,
        max_size=32 * 1024 * 1024,  # snapshots can exceed 1 MB
    ) as ws:
        hello = await call(ws, "agent.hello", token=token)
        print(f"connected: shotcut={hello['shotcutVersion']} mlt={hello['mltVersion']}")
        print(f"capabilities: {hello['capabilities']}")

        await call(ws, "agent.subscribe",
                   {"topics": ["timeline", "player", "project"]},
                   token=token)

        if project_path:
            print(f"opening {project_path}")
            await call(ws, "project.open", {"path": project_path}, token=token)
            # The open is queued onto the GUI thread; give it a moment.
            await asyncio.sleep(1.0)

        state = await call(ws, "project.state", token=token)
        print(f"project.state: file={state.get('file')!r} dirty={state.get('dirty')}")

        await call(ws, "player.seek", {"position": 30}, token=token)
        snap = await call(ws, "player.snapshot",
                          {"format": "png", "width": 320},
                          token=token)
        png = base64.b64decode(snap["data"])
        out_path = "/tmp/shotcut-agent-snapshot.png"
        with open(out_path, "wb") as f:
            f.write(png)
        print(f"snapshot @ frame {snap['position']} → {out_path} "
              f"({snap['width']}x{snap['height']}, {len(png)} bytes)")

        tracks = await call(ws, "timeline.tracks", token=token)
        print(f"tracks: {len(tracks)}")
        if tracks:
            target_track = tracks[0]["index"]
            print(f"appending color bars clip to track {target_track}")
            await call(ws, "timeline.appendClip",
                       {"trackIndex": target_track, "mltXml": COLOR_BARS_XML},
                       token=token)
            await asyncio.sleep(0.5)
            await call(ws, "agent.undo", token=token)
            print("undone")
        else:
            print("no tracks; skipping append/undo demo")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("project", nargs="?", help="Path to a .mlt file to open")
    ap.add_argument("--uri", default="ws://127.0.0.1:5555/")
    ap.add_argument("--token", default=os.environ.get("SHOTCUT_AGENT_TOKEN", ""))
    args = ap.parse_args()
    asyncio.run(run(args.uri, args.project, args.token or None))


if __name__ == "__main__":
    main()
