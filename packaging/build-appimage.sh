#!/bin/bash
# Builds dist/Ruche-<version>-x86_64.AppImage.
# Needs the lean audio-only libmpv (see packaging/build-deps.sh) in $DEPS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$HOME/deps}
BUILD=$ROOT/build-appimage
APPDIR=$BUILD/AppDir
OUT=$ROOT/dist
QTPLUGINS=$(qtpaths6 --query QT_INSTALL_PLUGINS)
VERSION=$(sed -n 's/^project(ruche VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")

echo "== building Ruche $VERSION"
PKG_CONFIG_PATH=$DEPS/lib/pkgconfig cmake -S "$ROOT" -B "$BUILD/app" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON >/dev/null
cmake --build "$BUILD/app" -j"$(nproc)" --target ruche
rm -rf "$APPDIR"
DESTDIR=$APPDIR cmake --install "$BUILD/app" >/dev/null
strip --strip-unneeded "$APPDIR/usr/bin/ruche"

echo "== runtime and libproxy stand-in"
gcc -O2 -Wall -Wno-format-truncation -I/usr/include/fuse3 -o "$BUILD/runtime" "$ROOT/packaging/runtime.c" \
  -Wl,-Bstatic -lsquashfuse_ll -lsquashfuse -lfuse3 -lzstd -llzma -lz -llz4 -llzo2 -Wl,-Bdynamic -lpthread -ldl
strip "$BUILD/runtime"
printf 'AI\002' | dd of="$BUILD/runtime" bs=1 seek=8 conv=notrunc status=none
mkdir -p "$APPDIR/usr/lib"
gcc -O2 -fPIC -shared -Wl,--version-script="$ROOT/packaging/libproxy-stub.map" -Wl,-soname,libproxy.so.1 \
  -o "$APPDIR/usr/lib/libproxy.so.1" "$ROOT/packaging/libproxy-stub.c"
cp -L "$DEPS/lib/libmpv.so.2" "$APPDIR/usr/lib/libmpv.so.2"
strip --strip-unneeded "$APPDIR/usr/lib/libmpv.so.2"

echo "== Qt plugins"
for p in platforms/libqxcb.so platforms/libqwayland-generic.so platforms/libqminimal.so \
         wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libbradient.so \
         platforminputcontexts/libcomposeplatforminputcontextplugin.so \
         imageformats/libqjpeg.so imageformats/libqsvg.so imageformats/libqwebp.so imageformats/libqico.so \
         imageformats/libqgif.so tls/libqopensslbackend.so; do
  mkdir -p "$APPDIR/usr/plugins/$(dirname $p)"
  cp "$QTPLUGINS/$p" "$APPDIR/usr/plugins/$p"
done
printf '[Paths]\nPrefix = ..\nPlugins = plugins\n' > "$APPDIR/usr/bin/qt.conf"

echo "== libraries"
python3 "$ROOT/packaging/bundle_libs.py" "$APPDIR" "$DEPS/lib"

echo "== desktop integration"
cp "$ROOT/packaging/AppRun" "$APPDIR/AppRun"
cp "$ROOT/packaging/ruche.desktop" "$APPDIR/ruche.desktop"
mkdir -p "$APPDIR/usr/share/applications" "$APPDIR/usr/share/icons/hicolor/256x256/apps" "$APPDIR/usr/share/metainfo"
cp "$ROOT/packaging/ruche.desktop" "$APPDIR/usr/share/applications/"
QT_QPA_PLATFORM=offscreen LD_LIBRARY_PATH="$DEPS/lib" "$BUILD/app/ruche" --render-icon "$APPDIR/ruche.png" 256
cp "$APPDIR/ruche.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/ruche.png"
ln -sf ruche.png "$APPDIR/.DirIcon"

echo "== image"
mkdir -p "$OUT"
rm -f "$BUILD/ruche.squashfs"
mksquashfs "$APPDIR" "$BUILD/ruche.squashfs" -root-owned -noappend -comp zstd -Xcompression-level 19 -quiet -no-progress
TARGET=$OUT/Ruche-$VERSION-x86_64.AppImage
cat "$BUILD/runtime" "$BUILD/ruche.squashfs" > "$TARGET"
chmod +x "$TARGET"
ls -la "$TARGET"
