#!/bin/bash
# Builds Ruche.exe and packages it as a portable zip and an installer:
#   dist/Ruche-<version>-windows-x64.zip, dist/Ruche-<version>-windows-x64-setup.exe
# Needs the dependencies from packaging/windows/build-deps-windows.sh in $WIN/prefix, and nsis.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
WIN=${WIN:-$HOME/win}
PREFIX=$WIN/prefix
HOSTQT=$WIN/qt-host
T=x86_64-w64-mingw32
BUILD=$ROOT/build-windows
OUT=$ROOT/dist
VERSION=$(sed -n 's/^project(ruche VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
NAME=Ruche-$VERSION-windows-x64
STAGE=$BUILD/$NAME
export WINPREFIX=$PREFIX PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig PKG_CONFIG_PATH=

echo "== building Ruche $VERSION for Windows"
cmake -S "$ROOT" -B "$BUILD/app" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-mingw64.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX/qt" -DQT_HOST_PATH="$HOSTQT" >/dev/null
cmake --build "$BUILD/app" --target ruche

echo "== staging"
rm -rf "$STAGE" && mkdir -p "$STAGE"
cp "$BUILD/app/Ruche.exe" "$STAGE/"
cp "$PREFIX/bin/libmpv-2.dll" "$STAGE/"
for m in Core Gui Widgets Network Svg; do cp "$PREFIX/qt/bin/Qt6$m.dll" "$STAGE/"; done
P=$PREFIX/qt/plugins
for p in platforms/qwindows.dll tls/qschannelbackend.dll \
         imageformats/qjpeg.dll imageformats/qgif.dll imageformats/qico.dll imageformats/qsvg.dll imageformats/qwebp.dll \
         iconengines/qsvgicon.dll; do
  [ -f "$P/$p" ] || { echo "missing plugin $p"; exit 1; }
  mkdir -p "$STAGE/$(dirname $p)" && cp "$P/$p" "$STAGE/$p"
done
cp -r "$ROOT/plugins" "$STAGE/plugins"
cp "$ROOT/PLUGINS.md" "$STAGE/PLUGINS.md"
cp -r "$ROOT/themes" "$STAGE/themes"
cp "$ROOT/THEMES.md" "$STAGE/THEMES.md"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cat > "$STAGE/README.txt" <<EOT
Ruche $VERSION for Windows

Run Ruche.exe. This folder is portable: your library, stations and settings live in
%LOCALAPPDATA%\\ruche, so they survive moving or replacing this folder.

Source code and licence: GPL-3.0-or-later (LICENSE.txt). libmpv is GPL; Qt and TagLib are LGPL.
EOT

# Copy every DLL the program and its plugins load that isn't part of Windows itself.
python3 - "$STAGE" "$PREFIX/qt/bin" "$PREFIX/bin" "/usr/lib/gcc/$T/$(${T}-gcc-posix -dumpversion)" "/usr/$T/lib" "/usr/$T/bin" <<'EOT'
import os, re, shutil, subprocess, sys
stage, search = sys.argv[1], sys.argv[2:]
system = {l.strip().lower() for l in """
kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll ole32.dll oleaut32.dll ws2_32.dll winmm.dll
imm32.dll comdlg32.dll uxtheme.dll dwmapi.dll version.dll shlwapi.dll netapi32.dll userenv.dll wtsapi32.dll
crypt32.dll secur32.dll ncrypt.dll bcrypt.dll iphlpapi.dll dnsapi.dll mpr.dll setupapi.dll authz.dll
d3d9.dll d3d11.dll d3d12.dll dxgi.dll dwrite.dll d2d1.dll avrt.dll ksuser.dll msvcrt.dll ntdll.dll
uiautomationcore.dll shcore.dll comctl32.dll winspool.drv opengl32.dll cfgmgr32.dll dxva2.dll propsys.dll
api-ms-win-core-synch-l1-2-0.dll powrprof.dll wininet.dll winhttp.dll rpcrt4.dll usp10.dll mf.dll mfplat.dll
winmm.dll mmdevapi.dll normaliz.dll wldap32.dll
""".split()}
def deps(path):
    out = subprocess.run(["x86_64-w64-mingw32-objdump", "-p", path], capture_output=True, text=True).stdout
    return re.findall(r"DLL Name: (\S+)", out)
todo = [os.path.join(r, f) for r, _, fs in os.walk(stage) for f in fs if f.lower().endswith((".exe", ".dll"))]
seen = {os.path.basename(p).lower() for p in todo}
missing = set()
while todo:
    for d in deps(todo.pop()):
        k = d.lower()
        if k in seen or k in system or k.startswith("api-ms-win-"):
            continue
        for s in search:
            c = os.path.join(s, d)
            if os.path.isfile(c):
                shutil.copy(c, os.path.join(stage, d))
                seen.add(k); todo.append(os.path.join(stage, d))
                break
        else:
            missing.add(d)
if missing:
    sys.exit("unresolved DLLs: " + ", ".join(sorted(missing)))
EOT
find "$STAGE" -name '*.dll' -o -name '*.exe' | xargs $T-strip --strip-unneeded

echo "== packages"
mkdir -p "$OUT"
rm -f "$OUT/$NAME.zip"
(cd "$BUILD" && 7z a -tzip -mx=9 "$OUT/$NAME.zip" "$NAME" >/dev/null)
(cd "$HERE" && makensis -V2 -DVERSION="$VERSION" -DSRCDIR="$STAGE" -DOUTFILE="$OUT/$NAME-setup.exe" installer.nsi)
ls -la "$OUT/$NAME.zip" "$OUT/$NAME-setup.exe"
