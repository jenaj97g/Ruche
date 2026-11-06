#!/bin/bash
# Builds dist/ruche_<version>-1_amd64.deb and dist/ruche-<version>-1.x86_64.rpm from the
# AppImage's AppDir, so the packages carry exactly the program the AppImage does (the lean
# libmpv and the Qt it was tested with), installed under /opt/ruche with /usr/bin/ruche,
# a menu entry and icons. Run packaging/build-appimage.sh first. Needs dpkg-dev and rpm.
#
# Same target systems as the AppImage (glibc 2.39+): Ubuntu 24.04+, Debian 13, Fedora 40+,
# openSUSE Tumbleweed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
APPDIR=$ROOT/build-appimage/AppDir
WORK=$ROOT/build-packages
OUT=$ROOT/dist
VERSION=$(sed -n 's/^project(ruche VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
REL=1
[ -x "$APPDIR/usr/bin/ruche" ] || { echo "run packaging/build-appimage.sh first"; exit 1; }

echo "== staging"
STAGE=$WORK/root
rm -rf "$WORK" && mkdir -p "$STAGE/opt/ruche" "$STAGE/usr/bin"
cp -a "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/usr/plugins" "$APPDIR/usr/share" "$STAGE/opt/ruche/"
# same start-up choices as the AppImage's AppRun
sed -e 's|^HERE=.*|HERE=/opt/ruche|' -e 's|\$HERE/usr/bin/ruche|$HERE/bin/ruche|' "$ROOT/packaging/AppRun" > "$STAGE/usr/bin/ruche"
chmod 755 "$STAGE/usr/bin/ruche"
install -Dm644 "$ROOT/packaging/ruche.desktop" "$STAGE/usr/share/applications/ruche.desktop"
install -Dm644 "$ROOT/packaging/ruche.svg" "$STAGE/usr/share/icons/hicolor/scalable/apps/ruche.svg"
for s in 16 22 24 32 48 64 128 256; do
  mkdir -p "$STAGE/usr/share/icons/hicolor/${s}x$s/apps"
  QT_QPA_PLATFORM=offscreen "$APPDIR/usr/bin/ruche" --render-icon "$STAGE/usr/share/icons/hicolor/${s}x$s/apps/ruche.png" $s 2>/dev/null
done
install -Dm644 "$ROOT/packaging/ruche.metainfo.xml" "$STAGE/usr/share/metainfo/ruche.metainfo.xml"
install -Dm644 "$ROOT/README.md" "$STAGE/usr/share/doc/ruche/README.md"
install -Dm644 "$ROOT/LICENSE" "$STAGE/usr/share/doc/ruche/LICENSE"
install -Dm644 "$ROOT/PLUGINS.md" "$STAGE/usr/share/doc/ruche/PLUGINS.md"
install -Dm644 "$ROOT/THEMES.md" "$STAGE/usr/share/doc/ruche/THEMES.md"
find "$STAGE" -type d -exec chmod 755 {} +
SIZE_KB=$(du -sk "$STAGE" | cut -f1)
mkdir -p "$OUT"

echo "== deb"
DEB=$WORK/deb
cp -a "$STAGE" "$DEB"
mkdir -p "$DEB/DEBIAN" "$WORK/shlib/debian"
# Host libraries the bundle needs, as Debian/Ubuntu package names
printf 'Source: ruche\n\nPackage: ruche\nArchitecture: amd64\n' > "$WORK/shlib/debian/control"
ELFS=$(find "$DEB/opt/ruche" -type f \( -name '*.so*' -o -path '*/bin/ruche' \) -exec sh -c 'file -b "$1" | grep -q ELF && echo "$1"' _ {} \;)
DEPENDS=$(cd "$WORK/shlib" && dpkg-shlibdeps -O --ignore-missing-info -l"$DEB/opt/ruche/lib" $ELFS 2>/dev/null \
          | sed -n 's/^shlibs:Depends=//p')
cat > "$DEB/DEBIAN/control" <<EOT
Package: ruche
Version: $VERSION-$REL
Architecture: amd64
Maintainer: jenaj97g <jenaj97g@users.noreply.github.com>
Installed-Size: $SIZE_KB
Depends: $DEPENDS
Section: sound
Priority: optional
Homepage: https://github.com/jenaj97g/Ruche
Description: light music player with a MusicBee-style layout and live radio titles
 Ruche browses your library by genre, artist and album, plays gapless through
 libmpv, and plays Icecast, Shoutcast and HLS radio with titles that follow the
 audio. Song notifications, MPRIS, close to tray.
EOT
dpkg-deb --root-owner-group -Zxz --build "$DEB" "$OUT/ruche_${VERSION}-${REL}_amd64.deb" >/dev/null

echo "== rpm"
RPMTOP=$WORK/rpm
mkdir -p "$RPMTOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
# The bundled libraries are private: don't advertise them, and don't ask the system for them.
BUNDLED=$(ls "$STAGE/opt/ruche/lib" | sed -e 's/\.so.*//' -e 's/[+.]/\\&/g' | paste -sd'|')
cat > "$RPMTOP/SPECS/ruche.spec" <<EOT
Name:           ruche
Version:        $VERSION
Release:        $REL
Summary:        Light music player with a MusicBee-style layout and live radio titles
License:        GPL-3.0-or-later
URL:            https://github.com/jenaj97g/Ruche
BuildArch:      x86_64
AutoReqProv:    yes
%global __provides_exclude_from ^/opt/ruche/.*\$
%global __requires_exclude ^(${BUNDLED})\\\\.so.*\$
%global debug_package %{nil}
%global __strip /bin/true
%global __brp_check_rpaths %{nil}
%global __brp_mangle_shebangs %{nil}
%global _build_id_links none

%description
Ruche browses your library by genre, artist and album, plays gapless through
libmpv, and plays Icecast, Shoutcast and HLS radio with titles that follow the
audio. Song notifications, MPRIS, close to tray.

%install
cp -a $STAGE/. %{buildroot}/

%files
/opt/ruche
/usr/bin/ruche
/usr/share/applications/ruche.desktop
/usr/share/icons/hicolor/*/apps/ruche.*
/usr/share/metainfo/ruche.metainfo.xml
%doc /usr/share/doc/ruche/README.md
%doc /usr/share/doc/ruche/PLUGINS.md
%doc /usr/share/doc/ruche/THEMES.md
%license /usr/share/doc/ruche/LICENSE
EOT
rpmbuild --quiet --define "_topdir $RPMTOP" -bb "$RPMTOP/SPECS/ruche.spec"
cp "$RPMTOP/RPMS/x86_64/ruche-$VERSION-$REL.x86_64.rpm" "$OUT/"
ls -la "$OUT/ruche_${VERSION}-${REL}_amd64.deb" "$OUT/ruche-$VERSION-$REL.x86_64.rpm"
