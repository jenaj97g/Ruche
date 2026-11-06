#!/bin/bash
# Builds the audio-only libmpv that the AppImage ships, from Ubuntu 24.04's own source
# packages (FFmpeg 6.1.1, libplacebo 6.338.2, mpv 0.37.0), into $DEPS.
#
# Ubuntu's libmpv links ~230 libraries (video codecs, Vulkan, speech synthesis, SVG,
# Samba, ...). Loading and relocating them costs ~30 MiB per process. This build keeps
# only audio decoders, the demuxers and protocols a music player needs, and links FFmpeg
# and libplacebo statically into libmpv: 23 libraries, all standard on a desktop.
#
# Needs: deb-src enabled, dpkg-dev nasm meson ninja-build libpipewire-0.3-dev
#        libasound2-dev libass-dev libssl-dev zlib1g-dev
set -euo pipefail
DEPS=${DEPS:-$HOME/deps}
SRC=${SRC:-$HOME/src}
mkdir -p "$DEPS" "$SRC"
cd "$SRC"
apt-get source ffmpeg libplacebo mpv >/dev/null

echo "== FFmpeg (audio only, static)"
cd "$SRC"/ffmpeg-6.1.1
./configure --prefix="$DEPS" --enable-static --disable-shared --enable-pic --enable-small --disable-debug \
  --disable-programs --disable-doc --disable-avdevice --disable-postproc --disable-autodetect --disable-everything \
  --enable-avcodec --enable-avformat --enable-avutil --enable-swresample --enable-swscale --enable-avfilter \
  --enable-openssl --enable-zlib --enable-network \
  --enable-decoder=mp3,mp3float,mp2,mp2float,mp1,mp1float,aac,aac_fixed,aac_latm,flac,vorbis,opus,alac,wavpack,ape,tta,wmav1,wmav2,wmapro,wmalossless,ac3,ac3_fixed,eac3,dca,truehd,mlp,mpc7,mpc8,als,tak,shorten,dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,adpcm_ima_wav,adpcm_ima_qt,adpcm_ms,pcm_s8,pcm_u8,pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_alaw,pcm_mulaw,pcm_s16le_planar,pcm_s24le_planar,pcm_s32le_planar \
  --enable-demuxer=mp3,aac,flac,ogg,mov,matroska,wav,w64,aiff,ape,wv,asf,mpc,mpc8,tta,hls,mpegts,ac3,eac3,dts,caf,dsf,loas,truehd,mlp,tak,shorten \
  --enable-parser=aac,aac_latm,ac3,flac,mpegaudio,opus,vorbis,dca,mlp,tak \
  --enable-protocol=file,http,https,httpproxy,tcp,tls,hls,crypto,data,pipe \
  --enable-filter=abuffer,abuffersink,aformat,anull,aresample,volume,buffer,buffersink,format,null,scale \
  --enable-bsf=null >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null

echo "== libplacebo (no GPU backends, static)"
cd "$SRC"/libplacebo-6.338.2
meson setup build --prefix="$DEPS" --libdir=lib --buildtype=release --default-library=static -Db_staticpic=true \
  -Dauto_features=disabled -Dvulkan=disabled -Dopengl=disabled -Dd3d11=disabled -Dglslang=disabled \
  -Dshaderc=disabled -Dlcms=disabled -Ddovi=disabled -Dlibdovi=disabled -Ddemos=false -Dtests=false >/dev/null
ninja -C build install >/dev/null

# Static archives: fold Libs.private into Libs so mpv links their dependencies too.
for f in "$DEPS"/lib/pkgconfig/lib{avcodec,avformat,avutil,avfilter,swscale,swresample,placebo}.pc; do
  python3 - "$f" <<'EOF'
import re, sys
p = sys.argv[1]; s = open(p).read()
m = re.search(r'^Libs\.private:(.*)$', s, re.M)
if m:
    s = re.sub(r'^Libs:(.*)$', lambda x: 'Libs:' + x.group(1) + ' ' + m.group(1).strip(), s, count=1, flags=re.M)
    s = re.sub(r'^Libs\.private:.*\n', '', s, flags=re.M)
open(p, 'w').write(s)
EOF
done

echo "== libmpv (audio only: PipeWire + ALSA outputs)"
cd "$SRC"/mpv-0.37.0
PKG_CONFIG_PATH="$DEPS/lib/pkgconfig" meson setup build --prefix="$DEPS" --libdir=lib --buildtype=release \
  -Doptimization=s -Dlibmpv=true -Dcplayer=false -Dauto_features=disabled -Dgl=disabled \
  -Dpipewire=enabled -Dalsa=enabled -Dpulse=disabled -Diconv=enabled -Dzlib=enabled \
  -Dlua=disabled -Djavascript=disabled -Dmanpage-build=disabled >/dev/null
ninja -C build install >/dev/null
echo "libmpv: $(ldd "$DEPS/lib/libmpv.so.2" | wc -l) linked libraries"
