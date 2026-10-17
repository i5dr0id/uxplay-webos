#!/usr/bin/env python3
"""Package the staged app into an IPK without ares-package."""
import json
import os
import pathlib
import shutil
import subprocess
import tarfile
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SDK = ROOT / '.cache/arm-webos-linux-gnueabi_sdk-buildroot'

def main():
    stage = ROOT / 'build/app'
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'lib').mkdir(parents=True)
    (stage / 'licenses').mkdir()
    for name in ['appinfo.json', 'icon.png']:
        shutil.copy2(ROOT / 'packaging' / name, stage / name)
    shutil.copy2(ROOT / 'build/uxplay-webos', stage / 'uxplay-webos')
    daemon = ROOT / 'build/uxplay-daemon'
    if daemon.exists():
        shutil.copy2(daemon, stage / 'uxplay-daemon')
    shutil.copy2(SDK / 'arm-webos-linux-gnueabi/sysroot/usr/lib/libstdc++.so.6', stage / 'lib/libstdc++.so.6')
    for source, dest in [
        ('LICENSE', 'GPL-3.0.txt'),
        ('.cache/openssl-3.5.8/LICENSE.txt', 'OpenSSL.txt'),
        ('.cache/libplist-2.7.0/COPYING', 'libplist.txt'),
        ('.cache/ffmpeg-8.1.2/COPYING.LGPLv2.1', 'FFmpeg.txt'),
        ('vendor/uxplay/lib/llhttp/LICENSE-MIT', 'llhttp.txt'),
        ('THIRD_PARTY.md', 'THIRD_PARTY.md'),
        ('packaging/GCC-RUNTIME-EXCEPTION.txt', 'GCC-RUNTIME-EXCEPTION.txt'),
    ]:
        src = ROOT / source
        if src.exists():
            shutil.copy2(src, stage / 'licenses' / dest)
    shutil.copy2(ROOT / 'dependencies.lock.json', stage / 'dependencies.lock.json')
    strip = SDK / 'bin/arm-webos-linux-gnueabi-strip'
    subprocess.run([str(strip), str(stage / 'uxplay-webos')], check=True)
    if (stage / 'uxplay-daemon').exists():
        subprocess.run([str(strip), str(stage / 'uxplay-daemon')], check=True)
    subprocess.run(['python3', str(ROOT / 'tools/verify-elf.py'), str(stage)], check=True)

    appinfo = json.loads((stage / 'appinfo.json').read_text())
    app_id = appinfo['id']
    version = appinfo['version']

    (ROOT / 'dist').mkdir(exist_ok=True)
    ipk_name = f'{app_id}_{version}_arm.ipk'
    ipk_path = ROOT / 'dist' / ipk_name

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)

        (tmp / 'debian-binary').write_text('2.0\n')

        control_text = (
            f'Package: {app_id}\n'
            f'Version: {version}\n'
            f'Section: misc\n'
            f'Priority: optional\n'
            f'Architecture: arm\n'
            f'Installed-Size: {sum(f.stat().st_size for f in stage.rglob("*") if f.is_file()) // 1024}\n'
            f'Description: {appinfo.get("title", app_id)}\n'
        )
        control_dir = tmp / 'control_staging'
        control_dir.mkdir()
        (control_dir / 'control').write_text(control_text)
        control_tar = tmp / 'control.tar.gz'
        with tarfile.open(str(control_tar), 'w:gz') as tar:
            tar.add(str(control_dir / 'control'), arcname='./control')

        install_prefix = f'usr/palm/applications/{app_id}'
        data_tar = tmp / 'data.tar.gz'
        with tarfile.open(str(data_tar), 'w:gz') as tar:
            for item in sorted(stage.rglob('*')):
                arcname = f'./{install_prefix}/{item.relative_to(stage)}'
                tar.add(str(item), arcname=arcname)

        with open(str(ipk_path), 'wb') as ipk:
            ipk.write(b'!<arch>\n')
            for member in ['debian-binary', 'control.tar.gz', 'data.tar.gz']:
                data = (tmp / member).read_bytes()
                name = member.encode().ljust(16)
                mtime = b'0'.ljust(12)
                uid = b'0'.ljust(6)
                gid = b'0'.ljust(6)
                mode = b'100644'.ljust(8)
                size = str(len(data)).encode().ljust(10)
                ipk.write(name + mtime + uid + gid + mode + size + b'`\n')
                ipk.write(data)
                if len(data) % 2:
                    ipk.write(b'\n')

    print(f'IPK: {ipk_path}')

if __name__ == '__main__':
    main()
