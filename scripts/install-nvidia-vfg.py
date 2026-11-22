#!/usr/bin/env python3
"""Install NVIDIA's pinned Linux VFG runtime, scoped to the current user."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import urllib.request
from zipfile import ZipFile

WHEEL = 'nvidia_vfx-0.2.0.0-cp312-abi3-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl'
URL = 'https://pypi.nvidia.com/nvidia-vfx/' + WHEEL
WHEEL_SHA = '0252866a7b798a6f92c8543443d88f716e8c5bbc6a7ddd77a2754728dada9fa8'
FEATURE = 'libnvVFXVideoFrameGeneration.so'
FEATURE_SHA = '9ceef5eee4a61ed7c7f27d48aeaf7cf46d7dbf5d6e4318d591bc5375996422d3'
AMPERE_FEATURE_SHA = '8b7cb0c7f3adc92914e1a1ba6fcba7ae587241bb1a9e0e92312df8cad3b137ed'
CUDA_RT = 'libcudart-256e6409.so.12.9.79'
LIBRARIES = ('libnppc.so.12', 'libnppial.so.12', 'libnppicc.so.12', 'libnppidei.so.12',
             'libnppig.so.12', 'libcudnn.so.9', 'libNVCVImage.so', 'libVideoFXLocal.so',
             'libVideoFX.so', 'libnvngxruntime.so', FEATURE)
MARKER = 'ready-v1'


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(4 * 1024**2), b''):
            digest.update(chunk)
    return digest.hexdigest()


def download(wheel):
    print('Downloading NVIDIA VFG (423 MiB)…', flush=True)
    with urllib.request.urlopen(URL, timeout=90) as response, wheel.open('wb') as output:
        received, last = 0, -1
        for data in iter(lambda: response.read(2 * 1024**2), b''):
            output.write(data)
            received += len(data)
            pct = min(received * 100 // 443100523, 100)
            if pct != last:
                print(f'NVIDIA VFG: {pct}%', flush=True)
                last = pct


def make_ampere_feature(native, destination):
    # Linux adaptation of the narrow architecture changes described in Veyra's
    # VfgAmpereCompat and RTX30-Broadcast-FrameGen (MIT; see docs/licenses).
    # Only this exact NVIDIA feature build is understood. Its original, the
    # system driver and every other effect are left untouched.
    image = bytearray(native.read_bytes())
    if hashlib.sha256(image).hexdigest() != FEATURE_SHA:
        raise ValueError('Unrecognized NVIDIA VFG module; SM86 adaptation stopped')
    # In this specific ELF build the dynamic string table has exactly one reference
    # to this function name. Redirect only this feature to our capability adapter.
    attr_offset = 218107447  # .dynstr (used by loader), not .strtab
    signature = b'cuDeviceGetAttribute\0'
    if image[attr_offset:attr_offset + len(signature)] != signature:
        raise ValueError('Unexpected VFG device attribute import')
    image[attr_offset:attr_offset + 2] = b'vx'

    magic = bytes.fromhex('50ed55ba01001000')
    pos, kernels = 0, 0
    while (pos := image.find(magic, pos)) >= 0:
        size, = struct.unpack_from('<Q', image, pos + 8)
        entry = pos + 16
        end = entry + size
        if size and size <= 32 * 1024**2 and end <= len(image):
            while entry < end:
                kind, version, header, payload = struct.unpack_from('<HHIQ', image, entry)
                body = entry + header
                if kind != 2 or version != 0x101 or header not in (64, 96) or not (64 <= payload <= end - body):
                    raise ValueError('Unexpected VFG kernel container')
                sm, = struct.unpack_from('<I', image, entry + 28)
                if sm == 89:
                    if (header != 64 or image[body:body + 6] != b'\x7fELF\x02\x01' or
                            struct.unpack_from('<H', image, body + 18)[0] != 190 or
                            struct.unpack_from('<I', image, body + 48)[0] != 0x00590559):
                        raise ValueError('Unrecognized VFG SM89 CUDA image')
                    struct.pack_into('<I', image, entry + 28, 86)
                    struct.pack_into('<I', image, body + 48, 0x00560556)
                    kernels += 1
                entry = body + payload
        pos += len(magic)
    if kernels != 62:
        raise ValueError(f'Expected 62 known SM89 kernel images; found {kernels}')
    if hashlib.sha256(image).hexdigest() != AMPERE_FEATURE_SHA:
        raise ValueError('Adapted VFG feature failed SHA-256 verification')
    destination.write_bytes(image)


def install(root, wheel_path):
    root.parent.mkdir(parents=True, exist_ok=True)
    with (root.parent / '.install-vfg.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if (root / MARKER).is_file():
            print('NVIDIA VFG is already installed.', flush=True)
            return
        if root.exists():
            raise ValueError(f'Incomplete or unrecognized installation at {root}. Move it aside to retry.')
        with tempfile.TemporaryDirectory(prefix='.vfg-', dir=root.parent) as folder:
            temp = Path(folder)
            wheel = wheel_path or temp / WHEEL
            if wheel_path is None:
                download(wheel)
            print('Verifying NVIDIA download…', flush=True)
            if sha256_file(wheel) != WHEEL_SHA:
                raise ValueError('NVIDIA download failed SHA-256 verification')
            bundle = temp / 'runtime'
            bundle.mkdir()
            with ZipFile(wheel) as archive:
                selected = [('nvvfx/libs/' + name, bundle / name) for name in LIBRARIES]
                selected.append(('nvidia_vfx.libs/' + CUDA_RT, bundle / CUDA_RT))
                selected.extend((name, bundle / 'licenses' / Path(name).name) for name in archive.namelist()
                                if '.dist-info/licenses/' in name and not name.endswith('/'))
                for member, output in selected:
                    output.parent.mkdir(exist_ok=True)
                    with archive.open(member) as source, output.open('wb') as target:
                        shutil.copyfileobj(source, target, 2 * 1024**2)
            print('Preparing the experimental RTX 30 runtime…', flush=True)
            ampere = bundle / 'ampere'
            ampere.mkdir()
            make_ampere_feature(bundle / FEATURE, ampere / FEATURE)
            for name in (*LIBRARIES, CUDA_RT):
                if name != FEATURE:
                    (ampere / name).symlink_to(Path('..') / name)
            (bundle / MARKER).write_text(WHEEL_SHA + '\n', encoding='ascii')
            bundle.rename(root)
    print('NVIDIA VFG installed. Reopen VeyAmux to test.', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wheel', type=Path, help='use an already downloaded, matching NVIDIA wheel')
    args = parser.parse_args()
    data = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share')
    install(data / 'veyamux-nvidia' / 'vfx-0.2.0.0', args.wheel)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
