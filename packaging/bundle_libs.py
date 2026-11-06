#!/usr/bin/env python3
"""Copies the shared libraries an AppDir needs into AppDir/usr/lib.

Libraries every Linux desktop ships (glibc, GL/X11/Wayland, fonts, audio servers, D-Bus,
OpenSSL...) are left to the host on purpose: they must match the running system.
Everything else is bundled and given an $ORIGIN-relative RUNPATH.
"""
import os
import re
import shutil
import subprocess
import sys

EXCLUDE = [
    # glibc and the compiler runtime (hosts with glibc >= 2.39 all ship GCC >= 13)
    r"ld-linux.*", r"libc\.so.*", r"libm\.so.*", r"libdl\.so.*", r"libpthread\.so.*", r"librt\.so.*",
    r"libresolv\.so.*", r"libutil\.so.*", r"libanl\.so.*", r"libmvec\.so.*", r"libnsl\.so.*",
    r"libstdc\+\+\.so.*", r"libgcc_s\.so.*",
    # graphics stack: must come from the host's drivers
    r"libGL\.so.*", r"libGLX\.so.*", r"libGLdispatch\.so.*", r"libOpenGL\.so.*", r"libEGL\.so.*",
    r"libGLESv2\.so.*", r"libdrm\.so.*", r"libgbm\.so.*", r"libvulkan\.so.*",
    # X11 / Wayland core
    r"libX11\.so.*", r"libX11-xcb\.so.*", r"libXau\.so.*", r"libXdmcp\.so.*", r"libXext\.so.*", r"libXrender\.so.*",
    r"libXfixes\.so.*", r"libXcursor\.so.*", r"libXi\.so.*", r"libXrandr\.so.*", r"libSM\.so.*", r"libICE\.so.*",
    r"libxcb\.so.*", r"libxcb-(randr|render|shape|shm|sync|xfixes|xkb|xinput|glx|dri2|dri3|present|xinerama)\.so.*",
    r"libxkbcommon\.so.*", r"libxkbcommon-x11\.so.*",
    r"libwayland-(client|cursor|egl|server)\.so.*",
    # fonts and text
    r"libfontconfig\.so.*", r"libfreetype\.so.*", r"libharfbuzz\.so.*", r"libgraphite2\.so.*", r"libexpat\.so.*",
    r"libpng16\.so.*", r"libbrotli.*",
    # audio servers
    r"libasound\.so.*", r"libpipewire-.*", r"libpulse.*", r"libjack\.so.*",
    # system services and very common base libraries
    r"libdbus-1\.so.*", r"libglib-2\.0\.so.*", r"libgobject-2\.0\.so.*", r"libgio-2\.0\.so.*", r"libgmodule-2\.0\.so.*",
    r"libgthread-2\.0\.so.*", r"libsystemd\.so.*", r"libudev\.so.*", r"libselinux\.so.*", r"libmount\.so.*",
    r"libblkid\.so.*", r"libpcre2-8\.so.*", r"libffi\.so.*", r"libz\.so.*", r"libzstd\.so.*", r"liblzma\.so.*",
    r"libbz2\.so.*", r"liblz4\.so.*", r"libcap\.so.*", r"libgcrypt\.so.*", r"libgpg-error\.so.*", r"libuuid\.so.*",
    r"libssl\.so.*", r"libcrypto\.so.*", r"libgssapi_krb5\.so.*", r"libkrb5\.so.*", r"libk5crypto\.so.*",
    r"libcom_err\.so.*", r"libkrb5support\.so.*", r"libkeyutils\.so.*", r"libbsd\.so.*", r"libmd\.so.*",
]
EXCLUDE_RE = re.compile("^(" + "|".join(EXCLUDE) + ")$")


def ldd(path, extra_paths):
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = ":".join(extra_paths)
    out = subprocess.run(["ldd", path], capture_output=True, text=True, env=env).stdout
    deps = {}
    for line in out.splitlines():
        m = re.match(r"\s*(\S+) => (\S+)", line)
        if m:
            deps[m.group(1)] = m.group(2)
        elif "not found" in line:
            deps[line.split()[0]] = None
    return deps


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def main(appdir, *extra_lib_dirs):
    libdir = os.path.join(appdir, "usr", "lib")
    os.makedirs(libdir, exist_ok=True)
    search = [libdir] + list(extra_lib_dirs)
    bundled, missing, skipped = set(os.listdir(libdir)), set(), set()
    changed = True
    while changed:
        changed = False
        elves = []
        for root, _, files in os.walk(os.path.join(appdir, "usr")):
            for f in files:
                p = os.path.join(root, f)
                if not os.path.islink(p) and is_elf(p):
                    elves.append(p)
        for elf in elves:
            for name, path in ldd(elf, search).items():
                if name in bundled:
                    continue
                if EXCLUDE_RE.match(name):
                    skipped.add(name)
                    continue
                if path is None:
                    missing.add(name)
                    continue
                shutil.copy2(os.path.realpath(path), os.path.join(libdir, name))
                os.chmod(os.path.join(libdir, name), 0o755)
                bundled.add(name)
                changed = True
    # $ORIGIN-relative search paths everywhere
    for root, _, files in os.walk(os.path.join(appdir, "usr")):
        for f in files:
            p = os.path.join(root, f)
            if os.path.islink(p) or not is_elf(p):
                continue
            rel = os.path.relpath(libdir, root)
            rpath = "$ORIGIN" if rel == "." else "$ORIGIN/" + rel
            subprocess.run(["patchelf", "--set-rpath", rpath, p], check=True)
    missing -= bundled
    print("bundled %d libraries; left to the host: %s" % (len(bundled), " ".join(sorted(skipped))))
    if missing:
        print("MISSING: " + " ".join(sorted(missing)))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(*sys.argv[1:]))
