#!/bin/bash
# Build a portable Shotcut .txz inside the official build container.
# Mirrors .github/workflows/build-linux.yml, but pulls source from a fork
# branch instead of upstream.
#
# Output: scripts/shotcut.txz and scripts/src.txz

set -euo pipefail

IMAGE_NAME="${IMAGE_NAME:-mltframework/shotcut-build:qt6.10.3-ubuntu22.04}"
BRANCH="${BRANCH:-claude/add-websocket-control-server-NJiW7}"
VERSION="${VERSION:-$(date +%y.%-m.%-d)}"

cd "$(dirname "$0")"

cat > build-shotcut.conf <<EOF
CLEANUP=0
SHOTCUT_VERSION="$VERSION"
SHOTCUT_HEAD=0
SHOTCUT_REVISION="origin/$BRANCH"
EOF

echo "Building Shotcut $VERSION from origin/$BRANCH using $IMAGE_NAME"
docker run --rm -v "$PWD:/root/shotcut" "$IMAGE_NAME"

mv shotcut.txz "shotcut-linux-x86_64-$VERSION.txz"
mv src.txz "shotcut-src-$VERSION.txz"

echo
echo "Done:"
ls -lh "shotcut-linux-x86_64-$VERSION.txz" "shotcut-src-$VERSION.txz"
