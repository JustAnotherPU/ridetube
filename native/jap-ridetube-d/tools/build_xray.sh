#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fetch the pinned official Xray-core Linux amd64 release and verify GitHub's published SHA-256.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
py=${1:-${PYTHON:-python3.12}}
VERSION=v26.11.9
SHA256=b651f7e1d9966ee2c1b1ee1bf8b52c72771824d3065dce96bbc8bcc19ed95f82
URL="https://github.com/XTLS/Xray-core/releases/download/$VERSION/Xray-linux-64.zip"
mkdir -p "$here/build/download"
zip="$here/build/download/xray-$VERSION-linux-64.zip"
sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
if [ ! -f "$zip" ] || [ "$(sha "$zip")" != "$SHA256" ]; then
    curl -fL --retry 3 -o "$zip.part" "$URL"
    mv "$zip.part" "$zip"
fi
[ "$(sha "$zip")" = "$SHA256" ] || { echo "build_xray.sh: SHA-256 mismatch" >&2; exit 1; }
mkdir -p "$here/build"
"$py" - "$zip" "$here/build/xray.tmp" <<'PY'
import os, sys, zipfile
src, out = sys.argv[1:]
with zipfile.ZipFile(src) as z:
    matches = [n for n in z.namelist() if n.rstrip('/').split('/')[-1] == 'xray' and not n.endswith('/')]
    if len(matches) != 1:
        raise SystemExit('build_xray.sh: expected one xray executable in release archive')
    info = z.getinfo(matches[0])
    if info.file_size < 1024 * 1024 or info.file_size > 100 * 1024 * 1024:
        raise SystemExit('build_xray.sh: unexpected xray binary size')
    with z.open(info) as inp, open(out, 'wb') as dst:
        while True:
            b = inp.read(1024 * 1024)
            if not b: break
            dst.write(b)
os.chmod(out, 0o755)
PY
mv "$here/build/xray.tmp" "$here/build/xray"
"$here/build/xray" version | grep -q "$VERSION" || { echo "build_xray.sh: wrong Xray version" >&2; exit 1; }
