#!/bin/sh
# Build libcai.dylib with CMake (Ninja, arm64, RelWithDebInfo) into
# mac/native/build/, then deploy it next to the Steam launch wrapper.
# Dependencies (prism, miniaudio, SimpleIni) are downloaded by CMake on the
# first configure. Requires Xcode command line tools, cmake and ninja.
set -e
DEV="$(cd "$(dirname "$0")" && pwd)"
cd "$DEV/../native"
cmake --preset default
cmake --build --preset default
"$DEV/deploy.sh"
