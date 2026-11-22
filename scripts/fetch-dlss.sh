#!/usr/bin/env bash
# Materialize only the licensed NVIDIA release SR feature, link stub and public headers.
set -euo pipefail
rgit=https://github.com/NVIDIA/DLSS.git
revision=9981c6350dc58c0ce21f024fd65e31e851c31689
root=$(cd "$(dirname "$0")/.." && pwd)
dest="${1:-$root/.tools/dlss}"
mkdir -p "$dest"
if [[ ! -d "$dest/.git" ]]; then
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$rgit"
fi
git -C "$dest" sparse-checkout init --no-cone
git -C "$dest" sparse-checkout set --no-cone '/include/' '/LICENSE.txt' '/lib/Linux_x86_64/libnvsdk_ngx.a' '/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16'
git -C "$dest" fetch --quiet --depth=1 --filter=blob:none origin "$revision"
git -C "$dest" checkout --quiet --detach "$revision"
(cd "$dest" && sha256sum --check --strict <<'SHA256'
d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df  lib/Linux_x86_64/libnvsdk_ngx.a
30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6  lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16
SHA256
)
