#!/bin/sh
set -eu
sh ./test.sh
cd build/linux
cpack -G 'DEB;RPM;TGZ;TXZ;ZIP'
version=$(sed -n 's/^set(CPACK_PACKAGE_VERSION "\([^"]*\)")$/\1/p' CPackConfig.cmake)
test -n "$version"
sha256sum UltimateAutoClicker-"$version"-Linux-* > SHA256SUMS-linux.txt
cd ../..
sh tests/packages.sh
