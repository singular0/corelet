#!/usr/bin/env bash
#
# Build .deb packages for the uConsole (arm64) and for x86 desktops (amd64).
#
# On a Debian machine with no arguments this is a plain dpkg-buildpackage run;
# that is the path CI takes on each architecture's own runner, and the one to
# use on the uConsole itself. Naming an architecture instead sets up a
# debian:trixie container and runs the same native path inside it, which is how
# a Mac gets either package: a foreign architecture runs under the qemu binfmt
# handler Docker ships, so it is slow but needs no cross toolchain.
#
# There is deliberately no cross-compilation path. It would be faster than
# qemu, but debhelper skips dh_auto_test whenever the host architecture differs
# from the build one, so a cross-built package ships untested -- and Qt would
# need its host tools pointed at separately. Two divergent build paths to save
# a few minutes on a package this size is a bad trade.
#
# Packages land in dist/. Every build happens in a temporary staged tree: that
# is where the Git-derived manifest and matching changelog stanza are injected,
# so dpkg can use them without modifying the checkout.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/dist
image=debian:trixie
cleanup_stage=

cleanup() {
    if [ -n "$cleanup_stage" ] && [ -d "$cleanup_stage" ]; then
        rm -rf "$cleanup_stage"
    fi
}

die() {
    echo "build-deb: $*" >&2
    exit 1
}

usage() {
    cat >&2 <<'EOF'
Usage: scripts/build-deb.sh [native | deps | amd64 | arm64 | all]

  native   build for this machine with dpkg-buildpackage (the default on
           Debian; this is what to run on the uConsole itself)
  deps     install the build dependencies on this machine, then stop
  amd64    build an x86-64 package in a debian:trixie container
  arm64    build an aarch64 package in a debian:trixie container
  all      both container builds

Output goes to dist/.
EOF
    exit 2
}

manifest_value() {
    local field=$1 manifest=$2
    sed -n "s/^set($field \"\\(.*\\)\")$/\\1/p" "$manifest"
}

resolve_version() {
    local source=$1 manifest=$2
    cmake -DSOURCE_DIR="$source" -DOUTPUT_MANIFEST="$manifest" \
        -P "$source/cmake/version.cmake"
}

# Everything that turns a stock Debian machine into one that can build the
# package. Build-Depends is the single dependency list: apt-get reads it out of
# debian/control rather than having it repeated here, in CI, and in the
# container recipe below.
install_deps() {
    command -v apt-get >/dev/null || die "deps needs a Debian machine"
    local sudo=
    [ "$(id -u)" -eq 0 ] || sudo=sudo
    export DEBIAN_FRONTEND=noninteractive
    $sudo apt-get update -qq
    $sudo apt-get install -y --no-install-recommends build-essential dpkg-dev debhelper
    $sudo apt-get build-dep -y "$root"
}

build_native() {
    command -v dpkg-buildpackage >/dev/null \
        || die "dpkg-buildpackage not found; run '$0 deps', or name an architecture"

    local stage_root source manifest version display date arch maintainer current package actual
    stage_root=$(mktemp -d)
    [ -n "$stage_root" ] && [ -d "$stage_root" ] \
        || die "cannot create a temporary build tree"
    cleanup_stage=$stage_root
    trap cleanup EXIT
    source=$stage_root/corelet
    manifest=$stage_root/version.cmake

    if [ -n "${CORELET_VERSION_MANIFEST-}" ]; then
        [ -f "$CORELET_VERSION_MANIFEST" ] \
            || die "version manifest not found: $CORELET_VERSION_MANIFEST"
        cp "$CORELET_VERSION_MANIFEST" "$manifest"
    else
        resolve_version "$root" "$manifest"
    fi

    version=$(manifest_value CORELET_VERSION_DEBIAN "$manifest")
    display=$(manifest_value CORELET_VERSION "$manifest")
    date=$(manifest_value CORELET_VERSION_DATE "$manifest")
    [ -n "$version" ] && [ -n "$display" ] && [ -n "$date" ] \
        || die "the version resolver produced an incomplete manifest"
    arch=$(dpkg --print-architecture)

    mkdir -p "$source"
    cp -a "$root/." "$source/"
    rm -rf "$source/.git" "$source/build" "$source/dist"
    cp "$manifest" "$source/.corelet-version.cmake"

    # debhelper gets the binary-package version from the first changelog stanza.
    # Existing stanzas remain release history; a generated one is added only
    # when the frozen Git version is not already represented by the first.
    current=$(sed -n '1s/^corelet (\([^)]*\)).*/\1/p' "$source/debian/changelog")
    if [ "$current" != "$version" ]; then
        maintainer=$(sed -n 's/^Maintainer:[[:space:]]*//p' "$source/debian/control" | head -1)
        [ -n "$maintainer" ] || die "cannot read the package maintainer"
        {
            printf 'corelet (%s) unstable; urgency=medium\n\n' "$version"
            printf '  * Build from Git version %s.\n\n' "$display"
            printf ' -- %s  %s\n\n' "$maintainer" "$date"
            cat "$source/debian/changelog"
        } >"$source/debian/changelog.generated"
        mv "$source/debian/changelog.generated" "$source/debian/changelog"
    fi

    # dpkg-buildpackage builds serially unless told otherwise, which on a
    # four-core CM4 leaves three cores idle for the whole compile.
    export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-parallel=$(nproc)}"
    ( cd "$source" && dpkg-buildpackage -b -us -uc )

    mkdir -p "$out"
    # The build emits the package and a -dbgsym alongside it; the .changes and
    # .buildinfo describe an upload nobody is making.
    mv "$stage_root"/corelet*_"${version}"_"${arch}".*deb "$out/"
    package=$out/corelet_"${version}"_"${arch}".deb
    actual=$(dpkg-deb -f "$package" Version)
    [ "$actual" = "$version" ] \
        || die "package says $actual, version manifest says $version"
    ls -1 "$out"/corelet*_"${version}"_"${arch}".*deb
}

build_container() {
    local arch=$1
    command -v docker >/dev/null || die "docker is required to build for $arch"
    mkdir -p "$out"

    # The source is mounted read-only and copied inside: dpkg-buildpackage
    # writes into the tree and into its parent, and a macOS build/ directory
    # full of Mach-O objects would confuse the Linux build if it came along.
    # Git is resolved from the read-only mount after dependencies are present;
    # the resulting manifest then follows the source into the second, native
    # staging step. This makes local container packages just as identifiable as
    # native and CI packages.
    docker run --rm --platform "linux/$arch" \
        -v "$root:/src:ro" -v "$out:/out" \
        -e "HOST_OWNER=$(id -u):$(id -g)" \
        "$image" bash -euo pipefail -c '
            cp -a /src /work
            rm -rf /work/build /work/dist /work/.git
            /work/scripts/build-deb.sh deps
            git config --global --add safe.directory /src
            cmake -DSOURCE_DIR=/src -DOUTPUT_MANIFEST=/tmp/corelet-version.cmake \
                -P /src/cmake/version.cmake
            CORELET_VERSION_MANIFEST=/tmp/corelet-version.cmake \
                /work/scripts/build-deb.sh
            cp /work/dist/*.deb /out/
            chown "$HOST_OWNER" /out/corelet*.deb
        '
    ls -1 "$out"/corelet*_"$arch".*deb
}

case "${1-}" in
    ""|native) build_native ;;
    deps) install_deps ;;
    amd64|arm64) build_container "$1" ;;
    all) build_container amd64; build_container arm64 ;;
    *) usage ;;
esac
