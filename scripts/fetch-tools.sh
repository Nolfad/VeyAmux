#!/usr/bin/env bash
# Fetch the exact tools used for the Linux x86-64 AppImage build.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$ROOT/.tools"
mkdir -p "$T/appimage"

verified() {
  printf '%s  %s\n' "$2" "$1" | sha256sum --check --status
}

fetch() {
  local url="$1" file="$2" checksum="$3" tmp
  if [[ -f "$file" ]] && verified "$file" "$checksum"; then return; fi
  tmp=$(mktemp "$file.XXXXXX")
  if curl --fail --location --silent --show-error --retry 3 -o "$tmp" "$url" && verified "$tmp" "$checksum"; then
    mv -f "$tmp" "$file"
  else
    rm -f "$tmp"
    echo "Could not obtain a verified copy of $url" >&2
    return 1
  fi
}

release=https://github.com
dxc_sha=96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1
fetch "$release/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz" "$T/dxc.tgz" "$dxc_sha"
if [[ ! -x "$T/dxc/bin/dxc" || ! -f "$T/dxc/.$dxc_sha" ]]; then
  dxc_dir=$(mktemp -d "$T/dxc.XXXXXX")
  tar xzf "$T/dxc.tgz" -C "$dxc_dir"
  chmod +x "$dxc_dir/bin/dxc"
  touch "$dxc_dir/.$dxc_sha"
  rm -rf "$T/dxc"
  mv "$dxc_dir" "$T/dxc"
fi

wgpu_sha=cf614af80f23c6364a13f6569e7ae4ca7367ebf6062e3d3d6e80e205264636b4
fetch "$release/gfx-rs/wgpu-native/releases/download/v29.0.0.0/wgpu-linux-x86_64-release.zip" "$T/wgpu.zip" "$wgpu_sha"
if [[ ! -f "$T/wgpu/lib/libwgpu_native.a" || ! -f "$T/wgpu/.$wgpu_sha" ]]; then
  wgpu_dir=$(mktemp -d "$T/wgpu.XXXXXX")
  python3 -m zipfile -e "$T/wgpu.zip" "$wgpu_dir"
  touch "$wgpu_dir/.$wgpu_sha"
  rm -rf "$T/wgpu"
  mv "$wgpu_dir" "$T/wgpu"
fi

fetch "$release/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage" \
      "$T/appimage/linuxdeploy-x86_64.AppImage" c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
fetch "$release/linuxdeploy/linuxdeploy-plugin-qt/releases/download/1-alpha-20250213-1/linuxdeploy-plugin-qt-x86_64.AppImage" \
      "$T/appimage/linuxdeploy-plugin-qt-x86_64.AppImage" 15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
fetch "$release/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage" \
      "$T/appimage/appimagetool-x86_64.AppImage" ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
fetch "$release/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64" \
      "$T/appimage/runtime-x86_64" 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
chmod +x "$T/appimage/"*.AppImage "$T/appimage/runtime-x86_64"
echo "Verified tools in $T"
