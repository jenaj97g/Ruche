#!/bin/bash
# Builds dist/ruche-<version>.flatpak, a single-file Flatpak bundle you install with
#   flatpak install --user ruche-<version>.flatpak
# It carries the AppImage's program (its Qt and lean libmpv) on top of the plain
# org.freedesktop.Platform runtime, which flatpak fetches from Flathub on install.
# Run packaging/build-appimage.sh first. Needs: flatpak, ostree, patchelf.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
APPDIR=$ROOT/build-appimage/AppDir
APPID=fr.jenaj97g.Ruche
BRANCH=stable
RUNTIME_VERSION=24.08
VERSION=$(sed -n 's/^project(ruche VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
B=$ROOT/build-flatpak
OUT=$ROOT/dist/ruche-$VERSION.flatpak
[ -x "$APPDIR/usr/bin/ruche" ] || { echo "run packaging/build-appimage.sh first"; exit 1; }

echo "== staging $APPID"
rm -rf "$B" && mkdir -p "$B/app/files" "$B/app/export/share/applications"
F=$B/app/files
cp -a "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins" "$APPDIR/usr/share" "$F/"
mv "$F/bin/ruche" "$F/bin/ruche-bin" # qt.conf stays next to it

# The runtime has glibc, the compiler runtime, X11/Wayland, fontconfig, OpenSSL, D-Bus,
# PipeWire and ALSA. A few libraries the AppImage takes from the host may be missing from it
# or differ: bring Ubuntu's copies (they only need glibc and each other).
EXTRA="libICE.so.6 libSM.so.6 libbsd.so.0 libmd.so.0 libcap.so.2 libgcrypt.so.20 libgpg-error.so.0
       libgssapi_krb5.so.2 libk5crypto.so.3 libkrb5.so.3 libkrb5support.so.0 libkeyutils.so.1 libcom_err.so.2
       libsystemd.so.0 libgraphite2.so.3 libuuid.so.1"
for so in $EXTRA; do
  src=$(ldconfig -p | awk -v n="$so" '$1 == n && /x86-64/ {print $NF; exit}')
  [ -n "$src" ] || { echo "missing $so on this machine"; exit 1; }
  cp -L "$src" "$F/lib/$so"
  patchelf --set-rpath '$ORIGIN' "$F/lib/$so"
done

# Launcher: the AppImage's start-up choices, then the program.
sed -e 's|^HERE=.*|HERE=/app|' -e 's|"\$HERE/usr/bin/ruche"|"$HERE/bin/ruche-bin"|' "$ROOT/packaging/AppRun" > "$F/bin/ruche"
chmod 755 "$F/bin/ruche"

# Desktop entry, icons and metadata under the app id
sed -e "s/^Icon=ruche$/Icon=$APPID/" -e 's/^StartupWMClass=ruche$/StartupWMClass=ruche/' \
  "$ROOT/packaging/ruche.desktop" > "$B/app/export/share/applications/$APPID.desktop"
mkdir -p "$F/share/applications" && cp "$B/app/export/share/applications/$APPID.desktop" "$F/share/applications/"
for s in 16 22 24 32 48 64 128 256; do
  d=$B/app/export/share/icons/hicolor/${s}x$s/apps
  mkdir -p "$d"
  QT_QPA_PLATFORM=offscreen "$APPDIR/usr/bin/ruche" --render-icon "$d/$APPID.png" $s 2>/dev/null
done
install -Dm644 "$ROOT/packaging/ruche.svg" "$B/app/export/share/icons/hicolor/scalable/apps/$APPID.svg"
cp -r "$B/app/export/share/icons" "$F/share/"
sed -e "s|<id>ruche</id>|<id>$APPID</id>|" -e "s|<launchable type=\"desktop-id\">ruche.desktop</launchable>|<launchable type=\"desktop-id\">$APPID.desktop</launchable>|" \
  "$ROOT/packaging/ruche.metainfo.xml" > "$B/metainfo.xml"
install -Dm644 "$B/metainfo.xml" "$F/share/metainfo/$APPID.metainfo.xml"

cat > "$B/app/metadata" <<EOT
[Application]
name=$APPID
runtime=org.freedesktop.Platform/x86_64/$RUNTIME_VERSION
sdk=org.freedesktop.Sdk/x86_64/$RUNTIME_VERSION
command=ruche

[Context]
shared=network;ipc;
sockets=x11;wayland;fallback-x11;pulseaudio;
devices=dri;
filesystems=home:ro;xdg-music;xdg-download;/media:ro;/run/media:ro;/mnt:ro;xdg-run/pipewire-0:ro;xdg-run/discord-ipc-0;xdg-run/app/com.discordapp.Discord:create;xdg-run/.flatpak/dev.vencord.Vesktop:create;xdg-run/snap.discord:ro;

[Session Bus Policy]
org.freedesktop.Notifications=talk
org.kde.StatusNotifierWatcher=talk
org.kde.StatusNotifierItem-2-1=own
org.kde.StatusNotifierItem-2-2=own
org.mpris.MediaPlayer2.ruche=own
org.mpris.MediaPlayer2.ruche.*=own
EOT

echo "== repository and bundle"
desktop-file-validate "$B/app/export/share/applications/$APPID.desktop"
ostree init --mode=archive-z2 --repo="$B/repo" >/dev/null
flatpak build-export --arch=x86_64 --subject="Ruche $VERSION" "$B/repo" "$B/app" "$BRANCH"
mkdir -p "$ROOT/dist"
flatpak build-bundle --arch=x86_64 --runtime-repo=https://flathub.org/repo/flathub.flatpakrepo \
  "$B/repo" "$OUT" "$APPID" "$BRANCH"
ls -la "$OUT"
