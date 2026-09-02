#!/bin/sh
# Build the macOS release asset:
#   release/Civ-VI-Accessibility-Integration-<version>-macos.zip
# containing the mod folder (src without ideHelpers.lua and todo.md, as the
# Windows release packages it), libcai.dylib, steam_launch.sh, install.sh,
# uninstall.sh and the third-party license texts.
#
# The mod folder is taken from the committed tree (git archive HEAD), never
# from the working copy, so nothing untracked can end up in the package. The
# version comes from src/CivViAccess.modinfo, which the Windows release
# workflow rewrites from the release tag: run this on the release commit,
# after that bump. An optional argument names the version to package and
# must then match the modinfo: package.sh 1.5.0
#
# Signing (optional, for a public release):
#   CAI_SIGN_IDENTITY="Developer ID Application: Name (TEAMID)"  signs the dylib
#   CAI_NOTARY_PROFILE=<notarytool keychain profile>              notarizes it
# Without them the dylib keeps its ad-hoc signature, which works because the
# game disables library validation; install.sh clears the quarantine flag.
set -e
DEV="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$DEV/../.." && pwd)"
NATIVE="$ROOT/mac/native"
MOD_NAME="CivVi-Accessibility-Integration"

VERSION="$(sed -n 's/.*<Mod id="[^"]*" version="\([^"]*\)".*/\1/p' "$ROOT/src/CivViAccess.modinfo" | head -1)"
[ -n "$VERSION" ] || { echo "could not read the version from src/CivViAccess.modinfo" >&2; exit 1; }
if [ -n "$1" ] && [ "$1" != "$VERSION" ]; then
  echo "requested version $1 but src/CivViAccess.modinfo says $VERSION; bump the modinfo first" >&2; exit 1
fi

# The supported game build is spelled out in the installer and in the dylib;
# a game update must change both.
BUILD_SH="$(sed -n 's/^SUPPORTED_BUILD="\(.*\)"$/\1/p' "$ROOT/mac/install.sh")"
BUILD_CPP="$(sed -n 's/^constexpr const char\* kSupportedBuild = "\(.*\)";$/\1/p' "$NATIVE/entry.cpp")"
[ -n "$BUILD_SH" ] && [ "$BUILD_SH" = "$BUILD_CPP" ] || { echo "supported build differs: install.sh says '$BUILD_SH', entry.cpp says '$BUILD_CPP'" >&2; exit 1; }
NAME="Civ-VI-Accessibility-Integration-$VERSION-macos"
STAGE="$NATIVE/build/package/$NAME"
OUT_DIR="$ROOT/release"
ZIP="$OUT_DIR/$NAME.zip"

# --- Build --------------------------------------------------------------------
cd "$NATIVE"
echo "configuring (the first run downloads about 17 MB of dependencies)"
cmake --preset default
cmake --build --preset default
DYLIB="$NATIVE/build/libcai.dylib"

# --- Sign and notarize (optional) ---------------------------------------------
if [ -n "$CAI_SIGN_IDENTITY" ]; then
  codesign --force --options runtime --timestamp -s "$CAI_SIGN_IDENTITY" "$DYLIB"
  echo "signed with $CAI_SIGN_IDENTITY"
  if [ -n "$CAI_NOTARY_PROFILE" ]; then
    NZIP="$NATIVE/build/libcai-notarize.zip"
    rm -f "$NZIP"; ditto -c -k "$DYLIB" "$NZIP"
    xcrun notarytool submit "$NZIP" --keychain-profile "$CAI_NOTARY_PROFILE" --wait
    echo "notarized (a dylib cannot be stapled; the ticket is checked online)"
  fi
fi
codesign --verify --verbose=1 "$DYLIB"

# --- Stage --------------------------------------------------------------------
rm -rf "$STAGE"; mkdir -p "$STAGE/licenses" "$STAGE/$MOD_NAME"
git -C "$ROOT" archive HEAD src | tar -x -C "$STAGE/$MOD_NAME" --strip-components 1
rm -f "$STAGE/$MOD_NAME/ideHelpers.lua" "$STAGE/$MOD_NAME/todo.md"
cp "$DYLIB" "$STAGE/libcai.dylib"
cp "$ROOT/mac/steam_launch.sh" "$ROOT/mac/install.sh" "$ROOT/mac/uninstall.sh" "$STAGE/"
chmod +x "$STAGE"/*.sh

# Third-party notices: prism ships its NOTICE and LICENSES folder; miniaudio and
# SimpleIni carry their license text inside the header.
DEPS="$NATIVE/build/_deps"
cp "$DEPS/prism-src/NOTICE" "$STAGE/licenses/prism-NOTICE.txt"
cp -R "$DEPS/prism-src/LICENSES" "$STAGE/licenses/prism"
awk '/This software is available as a choice of the following licenses/{p=1} p' "$DEPS/miniaudio-src/miniaudio.h" > "$STAGE/licenses/miniaudio.txt"
[ -s "$STAGE/licenses/miniaudio.txt" ] || { echo "miniaudio license text not found" >&2; exit 1; }
awk '/@section licence MIT LICENCE/{p=1; next} p&&/DEALINGS IN THE SOFTWARE\./{print; exit} p' "$DEPS/simpleini-src/SimpleIni.h" | sed 's/^    //' > "$STAGE/licenses/SimpleIni.txt"
[ -s "$STAGE/licenses/SimpleIni.txt" ] || { echo "SimpleIni license text not found" >&2; exit 1; }

# --- Zip ----------------------------------------------------------------------
mkdir -p "$OUT_DIR"; rm -f "$ZIP"
(cd "$STAGE/.." && ditto -c -k --keepParent "$NAME" "$ZIP")
echo "packaged $ZIP ($(du -h "$ZIP" | cut -f1))"
