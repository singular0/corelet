#!/usr/bin/env bash
#
# Build a distributable Corelet.dmg for this Mac's architecture.
#
# The bundle CMake produces is only runnable on the machine that built it: it
# links straight into the Homebrew Qt prefix. macdeployqt copies the frameworks
# and plugins in and rewrites the load commands, which is the whole difference
# between a development build and something a stranger can download.
#
# The result is ad-hoc signed, not signed with a Developer ID. Ad-hoc is the
# floor rather than a nicety -- Apple Silicon refuses to execute a Mach-O with
# no signature at all -- but it does not satisfy Gatekeeper, so a downloaded
# build is quarantined and has to be allowed by hand. See the note printed at
# the end, and README.md.
#
# One DMG per architecture, no universal binary: a Homebrew Qt is thin, so a
# universal build would mean lipo-ing every framework by hand or switching to
# the official Qt installer. CI builds each architecture on a runner of that
# architecture instead, exactly as the Debian packages do.
#
# Output goes to dist/.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/dist
work=$root/build/dmg

die() {
    echo "build-dmg: $*" >&2
    exit 1
}

usage() {
    cat >&2 <<'EOF'
Usage: scripts/build-dmg.sh [dmg | deps]

  dmg    build, deploy Qt, ad-hoc sign and package (the default)
  deps   install the build dependencies with Homebrew, then stop

Set QT_PREFIX to use a Qt other than Homebrew's.

Output goes to dist/.
EOF
    exit 2
}

# CFBundleShortVersionString comes from project(... VERSION ...) via
# etc/Info.plist.in, so unlike the Debian package there is no second version to
# drift out of sync -- this only has to name the file.
project_version() {
    local version
    version=$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][^[:space:]]*\)[[:space:]]*$/\1/p' \
        "$root/CMakeLists.txt" | head -1)
    [ -n "$version" ] || die "cannot read the project version"
    echo "$version"
}

qt_prefix() {
    if [ -n "${QT_PREFIX-}" ]; then
        echo "$QT_PREFIX"
        return
    fi
    command -v brew >/dev/null || die "no Homebrew; set QT_PREFIX to a Qt 6 prefix"
    brew --prefix qt 2>/dev/null || die "Qt not installed; run '$0 deps'"
}

install_deps() {
    command -v brew >/dev/null || die "deps needs Homebrew"
    brew install cmake qt
}

# A dedicated build tree rather than the usual build/. The development bundle
# is meant to keep pointing at the Homebrew prefix -- it is what gets launched
# during development -- and deploying into it in place would leave a half
# self-contained bundle behind that the next incremental link only partly
# undoes. It also means a release never inherits whatever CMAKE_BUILD_TYPE the
# working tree happens to be configured with.
build_app() {
    local qt=$1
    cmake -S "$root" -B "$work/cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$qt" >/dev/null
    cmake --build "$work/cmake" -j"$(sysctl -n hw.ncpu)"
    ctest --test-dir "$work/cmake" --output-on-failure
}

# macdeployqt copies plugin directories wholesale, and Qt's only input-context
# plugin is a virtual keyboard linking the entire QML runtime -- some 13 MB of
# QtQuick and QtQml inside an app that is Widgets precisely so it never loads
# any of it. Neither a Mac nor the uConsole types into an on-screen keyboard.
#
# Dropping the plugin is what leaves those frameworks unreferenced; the sweep
# then collects whatever nothing else in the bundle links, and repeats, because
# collecting QtQuick is in turn what orphans QtQmlModels. Only Frameworks/ is
# swept: plugins are opened by name at runtime, so nothing links them and the
# same test would call every one of them garbage.
prune_unused() {
    local app=$1 item base refs swept=1
    rm -rf "$app/Contents/PlugIns/platforminputcontexts"
    # Homebrew builds the PDF image-format plugin even when QtPdf is not
    # installed. macdeployqt copies the plugin but cannot deploy its framework,
    # leaving a binary that dyld could never load and this app never uses.
    rm -f "$app/Contents/PlugIns/imageformats/libqpdf.dylib"

    while [ "$swept" -eq 1 ]; do
        swept=0
        for item in "$app/Contents/Frameworks"/*; do
            [ -e "$item" ] || continue
            base=$(basename "$item")
            # Collect first, match second, and match against a here-string
            # rather than a pipe. Feeding find straight into `grep -q` reads
            # better and is wrong: grep exits at the first hit, find dies of
            # SIGPIPE, and under `set -o pipefail` the pipeline then reports
            # failure for a framework that *is* referenced -- so every
            # referenced framework gets deleted and the app is gutted.
            #
            # A dylib names itself in its own otool output and a framework's
            # binary names its framework, so the candidate is excluded from the
            # scan or nothing would ever look unreferenced.
            # Homebrew installs Qt framework binaries read-only (0444), so an
            # executable-bit filter silently omits them and makes every
            # transitive dependency look unused. otool identifies Mach-O files
            # itself; errors for resources are harmless and discarded.
            refs=$(find "$app/Contents" -type f \
                ! -path "$item" ! -path "$item/*" \
                -exec otool -L {} + 2>/dev/null) || true
            if ! grep -qE "/$base( |/)" <<<"$refs"; then
                rm -rf "$item"
                swept=1
            fi
        done
    done
}

# macdeployqt decides which plugins to copy from which Qt libraries the binary
# links, and it says nothing when it guesses wrong. Both of these are loaded by
# name at runtime, so a missing one is not a link error: it is the app starting
# with no window, or history silently failing with "Driver not loaded".
check_plugins() {
    local app=$1
    [ -e "$app/Contents/PlugIns/platforms/libqcocoa.dylib" ] \
        || die "macdeployqt did not copy the cocoa platform plugin"
    [ -e "$app/Contents/PlugIns/sqldrivers/libqsqlite.dylib" ] \
        || die "macdeployqt did not copy the SQLite driver; history would not work"
}

# The failure this catches is the one that only shows up on someone else's Mac:
# a framework macdeployqt did not know to copy stays an absolute reference into
# the build machine's Homebrew prefix, and the DMG then runs nowhere else.
check_self_contained() {
    local app=$1 f id ref leaks=
    while IFS= read -r f; do
        # A dylib's LC_ID_DYLIB is its own identity, not a file it loads. Some
        # read-only Homebrew binaries retain an absolute ID after macdeployqt;
        # dependencies on them are still correctly rewritten into the bundle.
        id=$(otool -D "$f" 2>/dev/null | sed -n '2p')
        while IFS= read -r ref; do
            [ "$ref" = "$id" ] && continue
            case "$ref" in
                /usr/local/*|/opt/homebrew/*)
                    leaks="$leaks  $f: $ref"$'\n'
                    ;;
            esac
        done < <(otool -L "$f" 2>/dev/null \
            | sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
    done < <(find "$app/Contents" -type f)
    [ -z "$leaks" ] || die "bundle still links outside itself:
$leaks"
}

# The other half of that check, and what keeps prune_unused honest: a reference
# into the bundle that no longer resolves. Over-pruning cannot show up as a
# build error -- it is a dyld failure at launch on the user's machine -- so the
# sweep is only safe with this run after it.
check_no_dangling() {
    local app=$1 f id ref target missing=
    while IFS= read -r f; do
        id=$(otool -D "$f" 2>/dev/null | sed -n '2p')
        while IFS= read -r ref; do
            [ "$ref" = "$id" ] && continue
            target=
            case "$ref" in
                @executable_path/*)
                    target="$app/Contents/MacOS/${ref#@executable_path/}"
                    ;;
                @loader_path/*)
                    target="${f%/*}/${ref#@loader_path/}"
                    ;;
                @rpath/*)
                    # macdeployqt puts every non-system rpath dependency in the
                    # bundle's Frameworks directory.
                    target="$app/Contents/Frameworks/${ref#@rpath/}"
                    ;;
            esac
            if [ -n "$target" ] && [ ! -e "$target" ]; then
                missing="$missing  $f: $ref"$'\n'
            fi
        done < <(otool -L "$f" 2>/dev/null \
            | sed -n '2,$s/^[[:space:]]*\([^[:space:]]*\).*/\1/p')
    done < <(find "$app/Contents" -type f)
    [ -z "$missing" ] || die "bundle references files it does not contain:
$missing"
}

# Signing has to come after macdeployqt, which rewrites the load commands of
# everything it touches and would invalidate any seal applied first, and it has
# to go inside-out: signing a nested binary after its container breaks the
# container's signature.
#
# No --options runtime. The hardened runtime is what notarization requires; on
# an ad-hoc signature it only adds restrictions, and buys nothing back.
adhoc_sign() {
    local app=$1 f
    while IFS= read -r -d '' f; do
        codesign --force --timestamp=none --sign - "$f"
    done < <(find "$app/Contents/Frameworks" "$app/Contents/PlugIns" \
        -type f -name '*.dylib' -print0)
    while IFS= read -r -d '' f; do
        codesign --force --timestamp=none --sign - "$f"
    done < <(find "$app/Contents/Frameworks" -maxdepth 1 -name '*.framework' -print0)
    codesign --force --timestamp=none --sign - "$app"
    codesign --verify --deep --strict "$app"
}

# hdiutil rather than create-dmg: a prettier window costs an AppleScript round
# trip through Finder, which needs a logged-in GUI session and so is exactly
# what fails on a CI runner. A staging folder holding the app next to a symlink
# to /Applications gives the drag-to-install layout with nothing but the tools
# macOS already ships.
build_dmg() {
    local qt version arch app stage dmg
    qt=$(qt_prefix)
    [ -x "$qt/bin/macdeployqt" ] || die "no macdeployqt in $qt/bin"
    version=$(project_version)
    arch=$(uname -m)

    build_app "$qt"

    stage=$work/stage
    rm -rf "$stage"
    mkdir -p "$stage" "$out"
    cp -R "$work/cmake/Corelet.app" "$stage/"
    ln -s /Applications "$stage/Applications"

    app=$stage/Corelet.app
    # macdeployqt logs "Cannot resolve rpath" for plugins whose Qt module is
    # not installed (QtPdf, QtVirtualKeyboard) and then fails its own codesign
    # pass. Both are noise here: the unresolvable plugins are ones this app
    # never loads, and everything is re-signed below anyway.
    "$qt/bin/macdeployqt" "$app" -always-overwrite
    prune_unused "$app"
    check_plugins "$app"
    check_self_contained "$app"
    check_no_dangling "$app"
    # Signing goes last: every check above is free to move files, and removing
    # one from a signed bundle breaks its seal.
    adhoc_sign "$app"

    dmg=$out/Corelet-$version-$arch.dmg
    rm -f "$dmg"
    hdiutil create -quiet \
        -volname "Corelet $version" \
        -srcfolder "$stage" \
        -fs HFS+ \
        -format UDZO \
        -ov \
        "$dmg"

    echo
    shasum -a 256 "$dmg"
    ls -lh "$dmg"
    cat >&2 <<EOF

This build is ad-hoc signed, so Gatekeeper will refuse it on first launch.
Since macOS 15 the Control-click bypass no longer works; open it once from
System Settings > Privacy & Security > Open Anyway, or run

    xattr -dr com.apple.quarantine /Applications/Corelet.app
EOF
}

case "${1-}" in
    ""|dmg) build_dmg ;;
    deps) install_deps ;;
    *) usage ;;
esac
