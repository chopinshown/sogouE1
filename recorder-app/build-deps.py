"""Pinned, checksum-verified voice dependencies; builds into ignored build/deps."""
import hashlib
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent
TOOLS = ROOT.parent / '.tools'
BUILD = ROOT / 'build/deps'
ZIG = TOOLS / 'zig-aarch64-macos-0.14.1/zig'
SOURCES = [
    ('mbedtls-3.6.7', 'https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2', 'a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6'),
    ('curl-8.23.0', 'https://curl.se/download/curl-8.23.0.tar.xz', '8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5'),
    ('opus-1.5.2', 'https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz', '65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1'),
    ('libsamplerate-0.2.2', 'https://codeload.github.com/libsndfile/libsamplerate/tar.gz/refs/tags/0.2.2', '16e881487f184250deb4fcb60432d7556ab12cb58caea71ef23960aec6c0405a'),
]
BUILD.mkdir(parents=True, exist_ok=True)
TOOLS.mkdir(exist_ok=True)
for name, url, digest in SOURCES:
    archive = TOOLS / (name + '.archive')
    if not archive.exists():
        with urllib.request.urlopen(url, timeout=60) as response:
            archive.write_bytes(response.read())
    if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
        raise RuntimeError('Checksum mismatch: ' + str(archive))
    if not (TOOLS / name).is_dir():
        with tarfile.open(archive) as tar:
            for item in tar.getmembers():
                if not (TOOLS / item.name).resolve().is_relative_to(TOOLS.resolve()):
                    raise RuntimeError('Unsafe archive path')
                if item.issym() or item.islnk():
                    if not (TOOLS / item.name).parent.joinpath(item.linkname).resolve().is_relative_to(TOOLS.resolve()):
                        raise RuntimeError('Unsafe archive link')
            tar.extractall(TOOLS)
for tool, args in [('cc', 'cc -target aarch64-linux-gnu.2.26'), ('ar', 'ar'), ('ranlib', 'ranlib')]:
    path = BUILD / tool
    path.write_text('#!/bin/sh\nexec "' + str(ZIG) + '" ' + args + ' "$@"\n')
    path.chmod(0o755)
toolchain = BUILD / 'toolchain.cmake'
toolchain.write_text(f'''set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER "{BUILD / 'cc'}")
set(CMAKE_AR "{BUILD / 'ar'}")
set(CMAKE_RANLIB "{BUILD / 'ranlib'}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_C_FLAGS_INIT "-O2 -fPIC")
''')
def build(name, options, artifact):
    dest = BUILD / name
    if (dest / artifact).exists():
        return
    subprocess.run(['cmake', '-S', str(TOOLS / name), '-B', str(dest),
                    '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DCMAKE_BUILD_TYPE=Release',
                    '-DBUILD_SHARED_LIBS=OFF', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', *options], check=True)
    subprocess.run(['cmake', '--build', str(dest), '-j', str(min(8, os.cpu_count() or 4))], check=True)
build('mbedtls-3.6.7', ['-DENABLE_TESTING=OFF', '-DENABLE_PROGRAMS=OFF'], 'library/libmbedtls.a')
mbed = BUILD / 'mbedtls-3.6.7/library'
build('curl-8.23.0', [
    '-DBUILD_CURL_EXE=OFF', '-DBUILD_TESTING=OFF', '-DBUILD_LIBCURL_DOCS=OFF', '-DBUILD_MISC_DOCS=OFF',
    '-DCURL_USE_MBEDTLS=ON', '-DCURL_USE_OPENSSL=OFF', '-DCURL_USE_PKGCONFIG=OFF', '-DCURL_USE_CMAKECONFIG=OFF',
    '-DMBEDTLS_INCLUDE_DIR=' + str(TOOLS / 'mbedtls-3.6.7/include'),
    '-DMBEDTLS_LIBRARY=' + str(mbed / 'libmbedtls.a'),
    '-DMBEDX509_LIBRARY=' + str(mbed / 'libmbedx509.a'),
    '-DMBEDCRYPTO_LIBRARY=' + str(mbed / 'libmbedcrypto.a'),
    '-DCURL_USE_LIBPSL=OFF', '-DCURL_USE_LIBSSH2=OFF', '-DCURL_USE_LIBSSH=OFF', '-DUSE_LIBIDN2=OFF',
    '-DCURL_ZLIB=OFF', '-DCURL_BROTLI=OFF', '-DCURL_ZSTD=OFF', '-DUSE_NGHTTP2=OFF', '-DUSE_NGTCP2=OFF',
    '-DHTTP_ONLY=ON', '-DCURL_DISABLE_WEBSOCKETS=OFF', '-DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt',
    '-DCURL_CA_PATH=none', '-DCURL_USE_LIBRTMP=OFF', '-DCURL_USE_GSSAPI=OFF',
], 'lib/libcurl.a')
build('opus-1.5.2', ['-DOPUS_BUILD_TESTING=OFF', '-DOPUS_BUILD_PROGRAMS=OFF', '-DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF'], 'libopus.a')
build('libsamplerate-0.2.2', ['-DBUILD_TESTING=OFF', '-DLIBSAMPLERATE_EXAMPLES=OFF', '-DLIBSAMPLERATE_INSTALL=OFF'], 'src/libsamplerate.a')
licenses = [(name, TOOLS / name / file) for name, file in [
    ('curl-8.23.0', 'COPYING'), ('mbedtls-3.6.7', 'LICENSE'),
    ('opus-1.5.2', 'COPYING'), ('libsamplerate-0.2.2', 'COPYING')]]
licenses.append(('cJSON 1.7.19', ROOT / 'vendor/cjson/LICENSE'))
(ROOT / 'build/voice-licenses.txt').write_text('\n\n'.join(name + '\n' + path.read_text() for name, path in licenses))
print('Voice dependencies ready')
