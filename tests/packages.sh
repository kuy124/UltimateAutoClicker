#!/bin/sh
set -eu
cd "$(dirname "$0")/../build/linux"
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
for archive in UltimateAutoClicker-*-Linux-*.tar.gz UltimateAutoClicker-*-Linux-*.tar.xz; do
    mkdir "$root/$archive"
    tar -xf "$archive" -C "$root/$archive"
done
unzip -q -o UltimateAutoClicker-*-Linux-*.zip -d "$root/zip"
dpkg-deb -x UltimateAutoClicker-*-Linux-*.deb "$root/deb"
rpm2cpio UltimateAutoClicker-*-Linux-*.rpm | (cd "$root" && cpio -id --quiet)
find "$root" -type f -name ultimate-autoclicker | while IFS= read -r binary; do
    test -x "$binary"
    "$binary" --version | grep '1.2.2 (Linux X11)'
    ldd "$binary" > "$root/ldd.txt"
    if grep -q 'not found' "$root/ldd.txt"; then cat "$root/ldd.txt"; exit 1; fi
done
test -f "$root/deb/usr/share/applications/ultimate-autoclicker.desktop"
dpkg-deb -f UltimateAutoClicker-*-Linux-*.deb Depends
rpm -qp --requires UltimateAutoClicker-*-Linux-*.rpm
sha256sum -c SHA256SUMS-linux.txt
echo 'All five Linux package formats extracted and verified.'
