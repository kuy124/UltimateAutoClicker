#!/bin/sh
set -eu
sh ./test.sh
cd build/linux
cpack -G 'DEB;RPM;TGZ;TXZ;ZIP'
sha256sum UltimateAutoClicker-1.2.2-Linux-* > SHA256SUMS-linux.txt
cd ../..
sh tests/packages.sh
