#!/bin/bash
# Cross-builds everything Ruche.exe needs, on Ubuntu 24.04, with mingw-w64 (posix threads):
#   Qt 6.8 (qtbase, qtsvg, qtimageformats), and the same lean, audio-only libmpv as the
#   AppImage (FFmpeg 6.1.1, libplacebo, mpv 0.37 from Ubuntu's source packages, plus the
#   libass/freetype/fribidi/harfbuzz that mpv insists on), TagLib 1.13 and SQLite.
#
# Needs: deb-src enabled, g++-mingw-w64-x86-64-posix mingw-w64-tools libz-mingw-w64-dev libvulkan-dev
#        cmake ninja-build meson nasm pkg-config git python3-jinja2
# Layout: $WIN/src (sources), $WIN/build, $WIN/qt-host (Qt tools for this machine),
#         $WIN/prefix (the Windows libraries).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WIN=${WIN:-$HOME/win}
QTVER=${QTVER:-6.8.3}
PREFIX=$WIN/prefix
HOSTQT=$WIN/qt-host
SRC=$WIN/src
B=$WIN/build
JOBS=${JOBS:-$(nproc)}
T=x86_64-w64-mingw32
export WINPREFIX=$PREFIX
mkdir -p "$SRC" "$B" "$PREFIX"

export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig
export PKG_CONFIG_PATH=
CROSS=$B/mingw64.meson
cat > "$CROSS" <<EOT
[binaries]
c = '$T-gcc-posix'
cpp = '$T-g++-posix'
ar = '$T-ar'
strip = '$T-strip'
windres = '$T-windres'
nasm = 'nasm'
pkg-config = 'pkg-config'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOT
meson_build() { # dir builddir [options...]
  local dir=$1 bd=$2 re=; shift 2
  [ -d "$bd/meson-info" ] && re=--reconfigure
  meson setup $re "$bd" "$dir" --cross-file "$CROSS" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    --default-library="${LIBTYPE:-static}" "$@" >/dev/null
  ninja -C "$bd" -j"$JOBS" install >/dev/null
}

step() { echo "== $*"; }

# ---------------------------------------------------------------- sources
cd "$SRC"
for m in qtbase qtsvg qtimageformats; do
  [ -d $m ] || git clone -q --depth 1 --branch v$QTVER https://github.com/qt/$m.git
done
[ -d taglib ] || git clone -q --depth 1 --branch v1.13.1 https://github.com/taglib/taglib.git
mkdir -p deb && cd deb
[ -d mpv-0.37.0 ] || apt-get source ffmpeg libplacebo mpv libass freetype fribidi harfbuzz >/dev/null

# ---------------------------------------------------------------- Qt
if [ ! -x "$HOSTQT/libexec/moc" ]; then
  step "Qt $QTVER host tools"
  mkdir -p "$B/qt-host" && cd "$B/qt-host"
  "$SRC/qtbase/configure" -prefix "$HOSTQT" -release -no-gui -no-widgets -no-dbus -no-opengl -no-icu -no-openssl \
    -nomake tests -nomake examples -no-feature-sql -no-feature-testlib -no-feature-concurrent -no-feature-xml -- -G Ninja >/dev/null
  ninja -j"$JOBS" >/dev/null && ninja install >/dev/null
fi
if [ ! -f "$PREFIX/qt/bin/Qt6Widgets.dll" ]; then
  step "Qt $QTVER for Windows (qtbase)"
  mkdir -p "$B/qtbase" && cd "$B/qtbase"
  "$SRC/qtbase/configure" -prefix "$PREFIX/qt" -qt-host-path "$HOSTQT" -xplatform win32-g++ -release -shared \
    -nomake tests -nomake examples -no-dbus -no-opengl -no-feature-vulkan -no-icu -no-openssl -schannel \
    -qt-zlib -qt-libpng -qt-libjpeg -qt-harfbuzz -qt-pcre -no-feature-sql -no-feature-testlib -no-feature-concurrent \
    -no-feature-printsupport -no-feature-xml \
    -- -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-mingw64.cmake" >/dev/null
  ninja -j"$JOBS" >/dev/null && ninja install >/dev/null
fi
for m in qtsvg qtimageformats; do
  if [ ! -d "$B/$m/.done" ]; then
    step "Qt $QTVER for Windows ($m)"
    mkdir -p "$B/$m" && cd "$B/$m"
    # Qt's own qt.toolchain.cmake chain-loads toolchain-mingw64.cmake
    "$PREFIX/qt/bin/qt-configure-module" "$SRC/$m" -- -DFEATURE_tiff=OFF -DFEATURE_jasper=OFF -DFEATURE_mng=OFF -DFEATURE_wbmp=OFF \
      -DFEATURE_tga=OFF -DFEATURE_icns=OFF -DFEATURE_macheif=OFF >/dev/null
    ninja -j"$JOBS" >/dev/null && ninja install >/dev/null
    mkdir -p "$B/$m/.done"
  fi
done

# ---------------------------------------------------------------- audio
cd "$SRC/deb"
if [ ! -f "$PREFIX/lib/libavcodec.a" ]; then
  step "FFmpeg (audio only, static, Schannel TLS)"
  rm -rf "$B/ffmpeg" && mkdir -p "$B/ffmpeg" && cd "$B/ffmpeg"
  "$SRC/deb/ffmpeg-6.1.1/configure" --prefix="$PREFIX" --target-os=mingw32 --arch=x86_64 --cross-prefix=$T- \
    --cc=$T-gcc-posix --cxx=$T-g++-posix --pkg-config=pkg-config \
    --enable-static --disable-shared --enable-small --disable-debug \
    --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-autodetect --disable-everything \
    --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale --enable-avfilter \
    --enable-schannel --enable-zlib --enable-network --enable-w32threads \
    --enable-decoder=mp3,mp3float,mp2,mp2float,mp1,mp1float,aac,aac_fixed,aac_latm,flac,vorbis,opus,alac,wavpack,ape,tta,wmav1,wmav2,wmapro,wmalossless,ac3,ac3_fixed,eac3,dca,truehd,mlp,mpc7,mpc8,als,tak,shorten,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,adpcm_ima_wav,adpcm_ima_qt,adpcm_ms,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_s16le_planar,pcm_s24le_planar,pcm_s32le_planar \
    --enable-demuxer=mp3,aac,flac,ogg,mov,matroska,wav,w64,aiff,ape,wv,asf,mpc,mpc8,tta,hls,mpegts,ac3,eac3,dts,caf,dsf,loas,truehd,mlp,tak,shorten \
    --enable-parser=aac,aac_latm,ac3,flac,mpegaudio,opus,vorbis,dca,mlp,tak \
    --enable-protocol=file,http,https,httpproxy,tcp,tls,hls,crypto,data,pipe \
    --enable-filter=abuffer,abuffersink,aformat,anull,aresample,volume,equalizer,buffer,buffersink,format,null,scale \
    --enable-bsf=null >/dev/null
  make -j"$JOBS" >/dev/null && make install >/dev/null
fi
cd "$SRC/deb"
if [ ! -f "$PREFIX/lib/libplacebo.a" ]; then
  step "libplacebo (no GPU backends, static)"
  # its Vulkan stubs include the Vulkan headers even with Vulkan off (header-only, from libvulkan-dev)
  mkdir -p "$PREFIX/include" && cp -r /usr/include/vulkan /usr/include/vk_video "$PREFIX/include/"
  meson_build libplacebo-6.338.2 "$B/placebo" -Dauto_features=disabled -Dvulkan=disabled -Dopengl=disabled \
    -Dd3d11=disabled -Dglslang=disabled -Dshaderc=disabled -Dlcms=disabled -Ddovi=disabled -Dlibdovi=disabled \
    -Ddemos=false -Dtests=false -Dc_args="-I$PREFIX/include"
fi
if [ ! -f "$PREFIX/lib/libfreetype.a" ]; then
  step "freetype, fribidi, harfbuzz (for libass, which mpv requires)"
  meson_build freetype-2.13.2+dfsg "$B/freetype" -Dbrotli=disabled -Dharfbuzz=disabled \
    -Dpng=disabled -Dzlib=internal
  meson_build fribidi-1.0.13 "$B/fribidi" -Ddocs=false -Dtests=false -Dbin=false
  meson_build harfbuzz-8.3.0 "$B/harfbuzz" -Dtests=disabled -Ddocs=disabled -Dglib=disabled -Dgobject=disabled \
    -Dcairo=disabled -Dicu=disabled -Dfreetype=disabled -Dutilities=disabled -Dintrospection=disabled \
    -Dchafa=disabled -Dgdi=disabled -Ddirectwrite=disabled -Dbenchmark=disabled
fi
if [ ! -f "$PREFIX/lib/libass.a" ]; then
  step "libass"
  rm -rf "$B/libass" && cp -r libass-0.17.1 "$B/libass" && cd "$B/libass"
  ./configure --host=$T --prefix="$PREFIX" --enable-static --disable-shared --disable-fontconfig \
    --disable-require-system-font-provider --disable-asm CC=$T-gcc-posix >/dev/null
  make -j"$JOBS" >/dev/null && make install >/dev/null
  cd "$SRC/deb"
fi
# Static archives: fold Libs.private into Libs so mpv links their dependencies too.
for f in "$PREFIX"/lib/pkgconfig/{libavcodec,libavformat,libavutil,libavfilter,libswscale,libswresample,libplacebo,libass,freetype2,fribidi,harfbuzz}.pc; do
  [ -f "$f" ] || continue
  python3 - "$f" <<'EOT'
import re, sys
p = sys.argv[1]; s = open(p).read()
m = re.search(r'^Libs\.private:(.*)$', s, re.M)
if m:
    s = re.sub(r'^Libs:(.*)$', lambda x: 'Libs:' + x.group(1) + ' ' + m.group(1).strip(), s, count=1, flags=re.M)
    s = re.sub(r'^Libs\.private:.*\n', '', s, flags=re.M)
m = re.search(r'^Requires\.private:(.*)$', s, re.M)
if m:
    s = re.sub(r'^Requires\.private:.*\n', '', s, flags=re.M)
    s = re.sub(r'^Requires:(.*)$', lambda x: 'Requires:' + x.group(1) + ', ' + m.group(1).strip(), s, count=1, flags=re.M) \
        if re.search(r'^Requires:', s, re.M) else s.rstrip('\n') + '\nRequires: ' + m.group(1).strip() + '\n'
if p.endswith('libplacebo.pc') and '-lstdc++' not in s:
    s = re.sub(r'^Libs:(.*)$', lambda x: 'Libs:' + x.group(1) + ' -lstdc++', s, count=1, flags=re.M)  # it has C++ parts
open(p, 'w').write(s)
EOT
done
if [ ! -f "$PREFIX/bin/libmpv-2.dll" ]; then
  step "libmpv (audio only: WASAPI output)"
  LIBTYPE=shared meson_build mpv-0.37.0 "$B/mpv" -Doptimization=s -Dlibmpv=true -Dcplayer=false \
    -Dauto_features=disabled -Dwin32-threads=enabled -Dwasapi=enabled -Dgl=disabled -Diconv=disabled -Dzlib=enabled \
    -Dlua=disabled -Djavascript=disabled -Dmanpage-build=disabled -Dc_link_args="-static"
  # -static keeps winpthreads inside libmpv-2.dll; it still imports zlib1.dll and libstdc++-6.dll
fi

# ---------------------------------------------------------------- tags, database
if [ ! -f "$PREFIX/lib/libtag.a" ]; then
  step "TagLib 1.13.1 (static)"
  cmake -S "$SRC/taglib" -B "$B/taglib" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-mingw64.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_BINDINGS=OFF -DWITH_ZLIB=ON >/dev/null
  ninja -C "$B/taglib" -j"$JOBS" install >/dev/null
  # static TagLib: consumers must not expect dllimport
  sed -i 's/^Cflags:\(.*\)$/Cflags:\1 -DTAGLIB_STATIC/' "$PREFIX/lib/pkgconfig/taglib.pc"
  grep -q -- '-lz' "$PREFIX/lib/pkgconfig/taglib.pc" || sed -i 's/^Libs:\(.*\)$/Libs:\1 -lz/' "$PREFIX/lib/pkgconfig/taglib.pc"
fi
if [ ! -f "$PREFIX/lib/libsqlite3.a" ]; then
  step "SQLite (the amalgamation Qt ships)"
  S=$SRC/qtbase/src/3rdparty/sqlite
  mkdir -p "$B/sqlite" "$PREFIX/include"
  $T-gcc-posix -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -c "$S/sqlite3.c" -o "$B/sqlite/sqlite3.o"
  $T-ar rcs "$PREFIX/lib/libsqlite3.a" "$B/sqlite/sqlite3.o"
  cp "$S/sqlite3.h" "$PREFIX/include/"
  V=$(sed -n 's/^#define SQLITE_VERSION *"\(.*\)"/\1/p' "$S/sqlite3.h")
  printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\n\nName: SQLite\nDescription: SQL database engine\nVersion: %s\nLibs: -L${libdir} -lsqlite3\nCflags: -I${includedir}\n' \
    "$PREFIX" "$V" > "$PREFIX/lib/pkgconfig/sqlite3.pc"
fi
echo "Windows dependencies are in $PREFIX"
