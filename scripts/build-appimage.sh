#!/usr/bin/env bash
# Builds VeyAmux-x86_64.AppImage.
# Needs (Ubuntu 24.04 names): cmake ninja-build g++ pkg-config qt6-base-dev qt6-declarative-dev qt6-svg-dev
#   qt6-wayland libvulkan-dev libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
#   libpulse-dev libva-dev qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts patchelf file
#   libssl-dev libcurl4-openssl-dev libjson-c-dev libminiupnpc-dev libevent-dev libopus-dev  (PlayStation Remote Play)
#   git (NVIDIA SDK)  python3 glslang-tools (AMD FidelityFX shader generation)  qt6-shader-baker (HDR shaders)  zsync
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"; APPDIR="$BUILD/AppDir"; OUT="${OUT_DIR:-$ROOT/dist}"
"$ROOT/scripts/fetch-tools.sh"
if [[ ! -f "$ROOT/.tools/dlss/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16" ]]; then
  "$ROOT/scripts/fetch-dlss.sh"
fi
T="$ROOT/.tools"; export APPIMAGE_EXTRACT_AND_RUN=1 NO_STRIP=1   # works without FUSE (containers/CI)

cmake -G Ninja -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DVEYAMUX_DXC_ROOT="$T/dxc" "$@"
cmake --build "$BUILD" --target veyamux veyamux-nvidia-vfg-probe
rm -rf "$APPDIR"; DESTDIR="$APPDIR" cmake --install "$BUILD"

QMAKE="$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake6)"; export QMAKE
export QML_SOURCES_PATHS="$ROOT/qml"
export EXTRA_PLATFORM_PLUGINS="libqwayland-generic.so;libqwayland-egl.so;libqoffscreen.so"
export EXTRA_QT_PLUGINS="svg;wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration;platforminputcontexts;xcbglintegrations"
export PATH="$T/appimage:$PATH"

# linuxdeploy rewrites the RPATH of every ELF file it sees. NGX loads the
# signed feature separately, so keep the original bytes outside its scan.
ngx="$APPDIR/usr/lib/veyamux/ngx/libnvidia-ngx-dlss.so"
if [[ -f "$ngx" ]]; then mv "$ngx" "$BUILD/ngx-release-library"; fi

# Pass 1: collect libraries + Qt plugins (no packaging yet).
"$T/appimage/linuxdeploy-x86_64.AppImage" --appdir "$APPDIR" --plugin qt \
  -d "$APPDIR/usr/share/applications/veyamux.desktop" -i "$APPDIR/usr/share/icons/hicolor/scalable/apps/veyamux.svg"
if [[ -d "${ngx%/*}" && -f "$BUILD/ngx-release-library" ]]; then
  mv "$BUILD/ngx-release-library" "$ngx"
fi

# The Vulkan loader and every GPU driver must come from the HOST (RADV/ANV/NVIDIA ICDs live there).
find "$APPDIR" \( -name 'libvulkan.so*' -o -name 'libGL.so*' -o -name 'libEGL.so*' -o -name 'libGLX*.so*' -o -name 'libOpenGL.so*' \
  -o -name 'libdrm*.so*' -o -name 'libgbm.so*' -o -name 'libwayland-*.so*' -o -name 'libsystemd.so*' -o -name 'libdbus-1.so*' \
  -o -name 'libva.so*' -o -name 'libva-*.so*' \) -delete
# libva too (instant replay / recording use VA-API): the host's libva knows where the host's VA driver lives
# (/usr/lib64/dri on Fedora/Bazzite, /usr/lib/x86_64-linux-gnu/dri on Debian/Ubuntu); a bundled one would not find it.

# NOTE: AppRun is a plain symlink to the binary, so env setup lives in main.cpp (not in apprun-hooks).

# Pass 2: build the AppImage from the cleaned AppDir (appimagetool does not re-resolve libraries).
mkdir -p "$OUT"
# Update information: AppImageUpdate / Gear Lever find new versions among the GitHub releases (upload the .zsync file
# next to the AppImage in every release).
UPDATE_INFO="${VEYAMUX_UPDATE_INFO:-none}"
# A local experimental build must not advertise replacement by an upstream binary.
update_args=()
if [[ "$UPDATE_INFO" != none ]]; then
  update_args=(-u "$UPDATE_INFO")
fi
( cd "$OUT" && ARCH=x86_64 "$T/appimage/appimagetool-x86_64.AppImage" --no-appstream --runtime-file "$T/appimage/runtime-x86_64" "${update_args[@]}" "$APPDIR" "VeyAmux-x86_64.AppImage" )
echo "Built: $OUT/VeyAmux-x86_64.AppImage"
