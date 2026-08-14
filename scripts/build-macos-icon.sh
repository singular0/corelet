#!/bin/sh

set -eu

if ! command -v magick >/dev/null 2>&1; then
    echo "ImageMagick is required (brew install imagemagick)." >&2
    exit 1
fi

if ! command -v swift >/dev/null 2>&1; then
    echo "Swift is required and is included with Xcode." >&2
    exit 1
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname -- "$script_dir")
source_png="$project_dir/assets/corelet.png"
macos_png="$project_dir/assets/corelet-macos.png"
icns="$project_dir/assets/corelet.icns"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/corelet-icon.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT HUP INT TERM

# Apple's macOS production grid uses an 824 px tile on a 1024 px canvas. The
# 185 px radius is applied to the tile, not the full canvas. Keeping the source
# artwork inside that footprint makes its apparent Dock size match system apps.
magick "$source_png" \
    -alpha on -trim +repage \
    -filter Lanczos -resize '824x824!' \
    -gravity center -background none -extent 1024x1024 \
    "$work_dir/artwork.png"

# Use the production-grid bounds directly. ImageMagick antialiases the curve,
# and the 100...923 pixel bounds produce the exact 824 px tile footprint.
magick -size 1024x1024 xc:none \
    -fill white -stroke none \
    -draw 'roundrectangle 100,100 923,923 185,185' \
    "$work_dir/mask.png"

magick "$work_dir/artwork.png" "$work_dir/mask.png" \
    -alpha off -compose CopyOpacity -composite \
    "$work_dir/tile.png"

# A restrained shadow gives the static ICNS the same lift as Apple's flattened
# system icons without changing the Corelet artwork itself.
magick "$work_dir/mask.png" \
    -channel A -blur 0x11 -evaluate multiply 0.42 +channel \
    -fill black -colorize 100 \
    "$work_dir/shadow.png"

magick -size 1024x1024 canvas:none \
    "$work_dir/shadow.png" -geometry +0+12 -compose Over -composite \
    "$work_dir/tile.png" -geometry +0+0 -compose Over -composite \
    -strip +set date:create +set date:modify \
    -define png:color-type=6 "$macos_png"

CLANG_MODULE_CACHE_PATH="$work_dir/clang-cache" \
SWIFT_MODULECACHE_PATH="$work_dir/swift-cache" \
swift "$script_dir/create-icns.swift" "$macos_png" "$work_dir/corelet.icns"
mv "$work_dir/corelet.icns" "$icns"
