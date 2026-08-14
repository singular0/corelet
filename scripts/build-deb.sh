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
# Packages land in dist/. dpkg-buildpackage writes its output next to the
# source tree, so the native path moves them out of the parent directory
# afterwards rather than leaving them there.

set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out=$root/dist
image=debian:trixie

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

# A native package's version is whatever debian/changelog says, so a stale
# entry silently mislabels the build. Compare it against the one source of
# truth without dpkg-parsechangelog, which does not exist on macOS.
check_version() {
    local project changelog
    project=$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][^[:space:]]*\)[[:space:]]*$/\1/p' \
        "$root/CMakeLists.txt" | head -1)
    changelog=$(sed -n '1s/^corelet (\([^)]*\)).*/\1/p' "$root/debian/changelog")
    [ -n "$project" ] && [ -n "$changelog" ] || die "cannot read the project version"
    if [ "$project" != "$changelog" ]; then
        echo "build-deb: warning: debian/changelog says $changelog," \
             "CMakeLists.txt says $project" >&2
    fi
    echo "$changelog"
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

    local version arch
    version=$(check_version)
    arch=$(dpkg --print-architecture)

    # dpkg-buildpackage builds serially unless told otherwise, which on a
    # four-core CM4 leaves three cores idle for the whole compile.
    export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-parallel=$(nproc)}"
    ( cd "$root" && dpkg-buildpackage -b -us -uc )

    mkdir -p "$out"
    # The build emits the package and a -dbgsym alongside it; the .changes and
    # .buildinfo describe an upload nobody is making.
    mv "$root"/../corelet*_"${version}"_"${arch}".*deb "$out/"
    rm -f "$root/../corelet_${version}_${arch}.changes" \
          "$root/../corelet_${version}_${arch}.buildinfo"
    ls -1 "$out"/corelet*_"${version}"_"${arch}".*deb
}

build_container() {
    local arch=$1
    command -v docker >/dev/null || die "docker is required to build for $arch"
    check_version >/dev/null
    mkdir -p "$out"

    # The source is mounted read-only and copied inside: dpkg-buildpackage
    # writes into the tree and into its parent, and a macOS build/ directory
    # full of Mach-O objects would confuse the Linux build if it came along.
    # .git goes with it, so the binary reports project(... VERSION ...) instead
    # of the tag it was built from -- these builds are one-offs for a machine
    # that cannot build natively, and the packages a release ships are built by
    # CI on a real checkout.
    # Past the copy this is the native path, unchanged.
    docker run --rm --platform "linux/$arch" \
        -v "$root:/src:ro" -v "$out:/out" \
        -e "HOST_OWNER=$(id -u):$(id -g)" \
        "$image" bash -euo pipefail -c '
            cp -a /src /work
            rm -rf /work/build /work/dist /work/.git
            /work/scripts/build-deb.sh deps
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
