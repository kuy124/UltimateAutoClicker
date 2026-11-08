#!/bin/sh
set -eu
sh ./build.sh
config=$(mktemp -d)
trap 'rm -rf "$config"' EXIT HUP INT TERM
export XDG_CONFIG_HOME="$config"
ctest --test-dir build/linux --output-on-failure
XDG_CONFIG_HOME="$config" xvfb-run -a build/linux/ultimate-autoclicker --selftest
