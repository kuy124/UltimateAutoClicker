#!/bin/sh
set -eu
cd "$(dirname "$0")/../build/linux"
version=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\([^"]*\)")$/\1/p' CPackConfig.cmake)
test -n "$version"
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT HUP INT TERM
for archive in UltimateAutoClicker-"$version"-Linux-*.tar.gz UltimateAutoClicker-"$version"-Linux-*.tar.xz; do
    mkdir "$root/$archive"
    tar -xf "$archive" -C "$root/$archive"
done
unzip -q -o UltimateAutoClicker-"$version"-Linux-*.zip -d "$root/zip"
dpkg-deb -x UltimateAutoClicker-"$version"-Linux-*.deb "$root/deb"
rpm2cpio UltimateAutoClicker-"$version"-Linux-*.rpm | (cd "$root" && cpio -id --quiet)
find "$root" -type f -name ultimate-autoclicker | while IFS= read -r binary; do
    test -x "$binary"
    "$binary" --version | grep -F "$version (Linux X11)"
    ldd "$binary" > "$root/ldd.txt"
    if grep -q 'not found' "$root/ldd.txt"; then cat "$root/ldd.txt"; exit 1; fi
done
test -f "$root/deb/usr/share/applications/ultimate-autoclicker.desktop"
dpkg-deb -f UltimateAutoClicker-"$version"-Linux-*.deb Depends
rpm -qp --requires UltimateAutoClicker-"$version"-Linux-*.rpm
sha256sum -c SHA256SUMS-linux.txt
echo 'All five Linux package formats extracted and verified.'
