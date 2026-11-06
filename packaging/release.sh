#!/bin/bash
# Builds every Ruche package into dist/ and writes dist/SHA256SUMS, the file the
# in-app updater checks downloads against. Then upload everything in dist/ for this
# version (and SHA256SUMS) to a GitHub release tagged v<version>.
#
#   DEPS=/path/to/linux-deps WIN=/path/to/windows-toolchain bash packaging/release.sh
#
# Leave WIN unset to skip the Windows build; SKIP_FLATPAK=1 skips the Flatpak bundle.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(ruche VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")
OUT=$ROOT/dist
mkdir -p "$OUT"
cd "$ROOT"

echo "== Ruche $VERSION"
python3 i18n/build.py
bash packaging/build-appimage.sh
bash packaging/build-packages.sh
[ "${SKIP_FLATPAK:-0}" = 1 ] || bash packaging/flatpak/build-flatpak.sh
if [ -n "${WIN:-}" ]; then
    bash packaging/windows/build-windows.sh
else
    echo "WIN not set: skipping the Windows build"
fi

# Asset names the updater looks for (see Updater.cpp)
files=()
for f in "Ruche-$VERSION-x86_64.AppImage" "ruche_$VERSION-1_amd64.deb" "ruche-$VERSION-1.x86_64.rpm" \
         "ruche-$VERSION.flatpak" "Ruche-$VERSION-windows-x64-setup.exe" "Ruche-$VERSION-windows-x64.zip"; do
    [ -f "$OUT/$f" ] && files+=("$f")
done
(cd "$OUT" && sha256sum "${files[@]}" > SHA256SUMS)

echo
echo "Upload these to a GitHub release tagged v$VERSION:"
for f in "${files[@]}" SHA256SUMS; do echo "  dist/$f"; done
if command -v gh >/dev/null; then
    echo
    echo "or, with the GitHub CLI:"
    echo "  gh release create v$VERSION --title \"Ruche $VERSION\" --notes-file <notes.md> \\"
    printf '    %s \\\n' "${files[@]/#/dist/}"
    echo "    dist/SHA256SUMS"
fi
