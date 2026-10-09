"""Loopback-only installer fixtures; never deployed with the website."""
import argparse
import hashlib
import io
import shutil
import tarfile
import zipfile
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = ROOT.parent.parent
SDK_ROOT = REPO_ROOT / 'dist'
FIXTURES = REPO_ROOT / 'out' / 'sdk-setup-tests' / 'fixtures'


def prepare():
    for name in ('vyx-sdk-windows-x86_64-llvm22.zip', 'vyx-sdk-linux-x86_64-llvm22.tar.gz'):
        source = SDK_ROOT / name
        root = name.removesuffix('.zip').removesuffix('.tar.gz')
        for variant in ('update', 'bad-startup'):
            folder = FIXTURES / variant / 'sdk'
            folder.mkdir(parents=True, exist_ok=True)
            dest = folder / name
            if not dest.exists():
                if name.endswith('.zip'):
                    if variant == 'update':
                        shutil.copyfile(source, dest)
                        with zipfile.ZipFile(dest, 'a') as archive:
                            archive.writestr(root + '/SETUP-TEST-REVISION', 'Synthetic archive revision; compiler unchanged.')
                    else:
                        with zipfile.ZipFile(dest, 'w') as archive:
                            archive.writestr(root + '/SDK-MANIFEST.json', '{}')
                            archive.writestr(root + '/bin/vyxc.exe', 'Not an executable.')
                else:
                    with tarfile.open(dest, 'w:gz', compresslevel=1) as archive:
                        if variant == 'update':
                            with tarfile.open(source) as original:
                                for member in original:
                                    archive.addfile(member, original.extractfile(member) if member.isfile() else None)
                            payload = b'Synthetic archive revision; compiler unchanged.'
                            member = tarfile.TarInfo(root + '/SETUP-TEST-REVISION')
                            member.size = len(payload)
                            archive.addfile(member, io.BytesIO(payload))
                        else:
                            for filename, payload in [('SDK-MANIFEST.json', b'{}'), ('bin/vyxc', b'#!/bin/sh\nexit 23\n')]:
                                member = tarfile.TarInfo(root + '/' + filename)
                                member.size = len(payload)
                                member.mode = 0o755
                                archive.addfile(member, io.BytesIO(payload))
            with dest.open('rb') as stream:
                digest = hashlib.file_digest(stream, 'sha256').hexdigest()
            dest.with_name(name + '.sha256').write_text(digest + '  ' + name + '\r\n')


class Handler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(ROOT), **kwargs)

    def translate_path(self, path):
        if path.startswith('/sdk/'):
            filename = path.removeprefix('/sdk/')
            if '/' not in filename and '\\' not in filename and filename.startswith('vyx-sdk-'):
                return str(SDK_ROOT / filename)
        return super().translate_path(path)

    def do_GET(self):
        if self.path.startswith('/bad-checksum/sdk/'):
            filename = self.path.rsplit('/', 1)[1]
            payload = ('0' * 64 + '  ' + filename.removesuffix('.sha256') + '\r\n').encode() if filename.endswith('.sha256') else b'corrupted SDK payload'
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        else:
            self.directory = str(FIXTURES if self.path.startswith(('/update/', '/bad-startup/')) else ROOT)
            super().do_GET()

    def log_message(self, *_):
        pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', type=int)
    parser.add_argument('--prepare', action='store_true')
    args = parser.parse_args()
    if args.prepare:
        prepare()
        print('Prepared synthetic installer revisions; real SDK compiler unchanged.', flush=True)
    if args.port:
        server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
        print(f'Installer fixtures: http://127.0.0.1:{args.port}', flush=True)
        server.serve_forever()
