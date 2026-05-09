---
name: shotcut-build
description: Build a portable Linux Shotcut .txz from this fork using the official mltframework Docker image. Use when the user asks to "build shotcut", "package shotcut", "build the docker container", "compile this fork", or otherwise wants the binary tarball produced from `scripts/build-docker.sh`. Covers the long incremental build, known failure signatures, and how to monitor without flooding context.
---

# Shotcut Docker build

Wraps `scripts/build-docker.sh`, which runs the official
`mltframework/shotcut-build:qt6.10.3-ubuntu22.04` image to compile every
dependency from source and produce `scripts/shotcut-linux-x86_64-<ver>.txz`.

## Quick start

```bash
cd /home/wes/Devel/shotcut
# Make sure all local commits the build needs are on origin (the fork).
# build-docker.sh pulls REVISION="origin/<branch>" inside the container.
git push origin "$(git rev-parse --abbrev-ref HEAD)"

# Kick off detached so the harness doesn't hold the process group.
rm -f scripts/build.log
setsid bash -c 'cd /home/wes/Devel/shotcut/scripts && ./build-docker.sh > build.log 2>&1 &' < /dev/null
docker ps --format "{{.ID}} {{.Status}}"   # confirm container is up
```

Then arm a Monitor on `scripts/build.log` (see "Monitoring" below). A full
build takes ~50–70 minutes on this machine. Output:

- `scripts/shotcut-linux-x86_64-<ver>.txz` — portable binary
- `scripts/shotcut-src-<ver>.txz` — source archive

Verify: `tar -xJf scripts/shotcut-linux-x86_64-<ver>.txz && Shotcut/Shotcut.app/bin/shotcut`

## Pre-flight checklist

Run these *before* launching the build — each one has bitten us in past
sessions:

1. **Push first.** The container clones from `https://github.com/wesnick/shotcut.git`
   and checks out `origin/<branch>`. Anything you only committed locally
   won't be in the build. `git push origin <branch>` first.
2. **Verify branch tip on origin.** `git ls-remote origin <branch>` should
   match local HEAD. If not, push.
3. **Disk space.** The build expands ~5 GB into `scripts/src/` and
   `scripts/Shotcut/`. Need at least ~10 GB free.
4. **No prior container running.** `docker ps` — if a previous build is
   still up, don't start a second one.
5. **Image present.** `docker images | grep shotcut-build` — if missing,
   the wrapper will pull it (~5 GB).

Most paths under `scripts/src/` and `scripts/Shotcut/` are **owned by root**
because Docker runs as root. You generally cannot delete or edit them from
the host as `wes`. The build script handles this internally; just don't try
to clean these dirs from the host without `sudo`.

Running `git status` / `git log` against any of `scripts/src/<lib>/` from
the host fails with `fatal: detected dubious ownership in repository`. **Do
not** add these to `git config --global safe.directory` — that bloats global
config with one entry per dep and isn't needed for the build. If you want
to inspect a sub-clone's state, do it inside the container:
`docker run --rm -v "$PWD/scripts:/root/shotcut" <image> -c 'cd /root/shotcut/src/<lib> && git log -3'`.

## Monitoring

Don't `tail -f` synchronously — the build is long. Use the Monitor tool with
a tight filter so you get phase transitions and real failure signals only:

```
tail -F /home/wes/Devel/shotcut/scripts/build.log 2>/dev/null | grep -E --line-buffered \
  "^LOG: Configuring, compiling|^LOG: Configuring [A-Za-z]|^LOG: Making [A-Za-z]|\
^LOG: Installing [A-Za-z]|^LOG: Process has finished|^LOG: Creating|\
^ERROR:|Reason: FAILURE|Reason: ABORTED|fatal error:|undefined reference|: error:|\
recipe for target.* failed|^make.*\\*\\*\\* "
```

Avoid filtering on `Compiling ` — CMake prints `-- Compiling a 64-bit binary.`
once per configure and floods notifications.

Use `timeout_ms: 3600000` (1 hour). If the build runs longer, re-arm.

## Build phase order

The script runs deps in this fixed order — knowing where you are helps
estimate time remaining:

```
OpenBLAS → whisper.cpp → libspatialaudio → libwebp → opencv → opencv_contrib
→ vmaf → SVT-AV1 → aom → dav1d → zimg → rubberband → vid.stab → ladspa
→ nv-codec-headers → opus → libvpx → x265_git → x264 → movit → frei0r
→ FFmpeg → mlt → shotcut → bigsh0t → gopro2gpx
```

`opencv` and `FFmpeg` are the longest. `shotcut` itself (the fork code) is
near the end — if a compile error in agent code ever surfaces, expect it
~50 minutes in.

## Known failure modes

The fixes for these are **already committed** on
`claude/add-websocket-control-server-NJiW7` (commits `83824985`, `abb1d4be`,
`5078605a`, `67fbe2b2`, `dea1fa56`). They live in `scripts/build-shotcut.sh`,
which is tracked. **If working on a different branch / on master, these
issues can resurface** — recognize the signatures:

### Source-fetch path (early)

- `error: pathspec 'master' did not match any file(s)` — the per-repo
  hardcoded `master` branch list is stale. Patched commit `83824985`
  auto-detects from `refs/remotes/origin/HEAD`.
- `git repository has local changes, aborting checkout` (typically on
  `ladspa`) — build-time debris (e.g. `metadata/swh-plugins.rdf`,
  `ABOUT-NLS`) tripping the `git diff-index` check that ran *before*
  `git reset --hard`. Patched commit `83824985` (reordered to reset first;
  removed the check). Do NOT add `git clean -fdx` here — that wipes
  generated `./configure` scripts (commit `abb1d4be` removed it).
- `error: pathspec 'origin/<branch>' did not match` after pull — the script
  used `git pull URL <main_branch>`, which doesn't refresh other refs.
  Commit `83824985` adds explicit `git fetch origin`.

### Compile path

- `ln: failed to create symbolic link 'libx265_main10.a': File exists` —
  x265 preconfig used bare `ln -s`. Commit `5078605a` made it `ln -sf`.
- `./configure: No such file or directory` for `zimg` — only happens if
  something wiped untracked files (e.g. accidental `git clean -fdx`).
  PRECONFIG[20]=`./autogen.sh` is set at script init based on
  `./configure` presence then; reset is too late to flip the flag. Don't
  re-clean untracked files.
- `x265 not found using pkg-config` while configuring FFmpeg — the cause
  is `libx265.so` exporting only namespaced symbols
  (`x265_8bit::x265_api_get_215`) and missing the top-level
  `x265_api_get_215`. Caused by stale CMake cache pollution where
  `HIGH_BIT_DEPTH=ON` and/or `EXPORT_C_API=OFF` got cached in
  `scripts/src/x265_git/source/CMakeCache.txt`. Commits `67fbe2b2` +
  `dea1fa56` pass both explicitly in `CONFIG[13]`. To verify:
  ```bash
  nm -D scripts/Shotcut/Shotcut.app/lib/libx265.so \
    | grep -E "^[0-9a-f]+ T x265_api_get_215\b"  # must return a hit
  ```

### Shotcut compile (late)

If the agent code is ever extended:

- `invalid use of incomplete type 'class QUndoStack'` → forward declaration
  only; add `#include <QUndoStack>` (commit `5b5a3bbf`).
- `Mlt::Service has no member named get_in/get_out` → those exist on
  `Mlt::Producer`/`Mlt::Filter`, not the base. Use the property API
  (`service.get_int("in")`, `service.get_int("out")`) — works for any
  service. (commit `5b5a3bbf`).

## When things go wrong mid-build

1. The build is **incremental** (`CLEANUP=0` in `build-shotcut.conf`). On
   re-run it will repeat all configure/compile steps, but it skips
   re-cloning sources and re-uses installed libs in `Shotcut/Shotcut.app/`.
   Each full pass is still ~50 min.
2. **Don't `rm -rf scripts/src` from the host** — root-owned. If you
   really need to nuke a source dir, do it inside docker:
   ```bash
   docker run --rm -v "$PWD/scripts:/root/shotcut" \
     mltframework/shotcut-build:qt6.10.3-ubuntu22.04 \
     -c 'rm -rf /root/shotcut/src/<lib>'
   ```
3. **Container teardown** — `docker run --rm` is fine. If a previous build
   left a container behind, `docker ps -a` then `docker rm <id>`.
4. **Push fixes, don't patch in place.** `scripts/src/shotcut/` is the
   container's clone of the fork. Editing files there gets blown away by
   `git reset --hard` on the next run. Always push to `origin/<branch>`.

## Configuration knobs

`scripts/build-docker.sh` honors three env vars:

- `IMAGE_NAME` — default `mltframework/shotcut-build:qt6.10.3-ubuntu22.04`.
- `BRANCH` — default `claude/add-websocket-control-server-NJiW7`. Change to
  build a different fork branch.
- `VERSION` — default `$(date +%y.%-m.%-d)`. Used in tarball filenames.

Example:
```bash
BRANCH=master VERSION=test ./scripts/build-docker.sh
```
