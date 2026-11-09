#!/usr/bin/env bash
# Builds the AppImages from a folder made by tools/package_linux.sh. The game is not included.
# Usage: tools/make_appimage.sh <release-folder> <output-dir> <version>   (needs appimagetool on PATH or APPIMAGETOOL)
set -euo pipefail
release=$(cd "$1" && pwd); mkdir -p "$2"; out=$(cd "$2" && pwd); version=$3
repo=$(cd "$(dirname "$0")/.." && pwd)
tool=${APPIMAGETOOL:-appimagetool}
export APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64
make_one() {  # name, file name suffix, "deck" or ""
    local dir; dir=$(mktemp -d)
    mkdir "$dir/payload"
    cp -r "$release/SonsOfSparta-PS5" "$release/libs" "$release/tools" "$release/LICENSE" "$dir/payload/"
    if [[ -f $release/README-LINUX.md ]]; then cp "$release/README-LINUX.md" "$dir/payload/"; fi
    printf '%s\n' "$version" > "$dir/payload/VERSION"
    cp "$repo/packaging/appimage/AppRun" "$dir/AppRun"
    cp "$repo/packaging/appimage/sons-of-sparta.png" "$dir/sons-of-sparta.png"
    if [[ $3 == deck ]]; then touch "$dir/STEAMDECK"; fi
    cat > "$dir/sons-of-sparta.desktop" <<D
[Desktop Entry]
Type=Application
Name=$1
Exec=AppRun
Icon=sons-of-sparta
Categories=Game;
D
    chmod -R a+rX "$dir"  # mktemp makes it private; appimagetool refuses that
    chmod a+rx "$dir/AppRun"
    "$tool" --no-appstream "$dir" "$out/SonsOfSparta-PS5-$2-$version-x86_64.AppImage"
    rm -rf "$dir"
}
make_one "Sons of Sparta" Linux ""
make_one "Sons of Sparta (Steam Deck)" SteamDeck deck
