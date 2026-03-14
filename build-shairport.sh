#!/bin/bash
# Cross-compile shairport-sync for ARM (Bose SoundTouch 20)
# Builds static binary with pipe backend + tinysvcmdns
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SPS_DIR="$(cd "$SCRIPT_DIR/../shairport-sync" && pwd)"
OUT="$SCRIPT_DIR/build/shairport-sync"
IMAGE="shairport-sync-build"

# Build Docker image with cross-compile tools
docker build --platform linux/amd64 -t "$IMAGE" -f - . <<'DOCKERFILE'
FROM --platform=linux/amd64 ubuntu:22.04
RUN apt-get update -qq && \
    apt-get install -y -qq \
      gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf make \
      autoconf automake libtool pkg-config \
      cmake wget ca-certificates && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /build
DOCKERFILE

mkdir -p "$SCRIPT_DIR/build"

# Run the entire cross-compilation inside Docker
docker run --rm --platform linux/amd64 \
  -v "$SPS_DIR:/shairport-sync:ro" \
  -v "$SCRIPT_DIR/build:/out" \
  "$IMAGE" bash -exc '
HOST=arm-linux-gnueabihf
PREFIX=/opt/arm-deps
export CC=${HOST}-gcc
export AR=${HOST}-ar
export RANLIB=${HOST}-ranlib
mkdir -p $PREFIX

# ── 1. libpopt ──────────────────────────────────────────
cd /build
wget -q --no-check-certificate https://ftp.rpm.org/popt/releases/popt-1.x/popt-1.19.tar.gz
tar xzf popt-1.19.tar.gz
cd popt-1.19
./configure --host=$HOST --prefix=$PREFIX \
  --enable-static --disable-shared --disable-nls \
  CFLAGS="-O2 -D_FILE_OFFSET_BITS=64"
make -j$(nproc)
make install
echo "=== libpopt done ==="

# ── 2. libconfig ────────────────────────────────────────
cd /build
wget -q --no-check-certificate https://github.com/hyperrealm/libconfig/releases/download/v1.7.3/libconfig-1.7.3.tar.gz
tar xzf libconfig-1.7.3.tar.gz
cd libconfig-1.7.3
./configure --host=$HOST --prefix=$PREFIX \
  --enable-static --disable-shared --disable-cxx \
  CFLAGS="-O2 -D_FILE_OFFSET_BITS=64"
make -j$(nproc)
make install
echo "=== libconfig done ==="

# ── 3. mbedtls ──────────────────────────────────────────
cd /build
wget -q --no-check-certificate https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
tar xjf mbedtls-3.6.2.tar.bz2
cd mbedtls-3.6.2
# mbedtls uses cmake
cmake -DCMAKE_C_COMPILER=${HOST}-gcc \
      -DCMAKE_AR=$(which ${HOST}-ar) \
      -DCMAKE_RANLIB=$(which ${HOST}-ranlib) \
      -DCMAKE_INSTALL_PREFIX=$PREFIX \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_SYSTEM_PROCESSOR=arm \
      -DENABLE_PROGRAMS=OFF \
      -DENABLE_TESTING=OFF \
      -DCMAKE_C_FLAGS="-O2 -D_FILE_OFFSET_BITS=64" \
      .
make -j$(nproc)
make install
echo "=== mbedtls done ==="

# ── 4. shairport-sync ──────────────────────────────────
# Copy source (mounted read-only)
cp -a /shairport-sync /build/shairport-sync
cd /build/shairport-sync
autoreconf -fi

# Point configure at our cross-compiled deps
export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig
export CFLAGS="-O2 -D_FILE_OFFSET_BITS=64 -I$PREFIX/include"
export LDFLAGS="-static -L$PREFIX/lib"

./configure \
  --host=$HOST \
  --with-os=linux \
  --with-ssl=mbedtls \
  --with-pipe \
  --with-stdout \
  --with-tinysvcmdns \
  --with-metadata \
  --without-configfiles \
  --without-pkg-config \
  LIBS="-lmbedx509 -lmbedcrypto"

make -j$(nproc)

cp shairport-sync /out/shairport-sync
${HOST}-strip /out/shairport-sync
echo "=== shairport-sync built ==="
ls -lh /out/shairport-sync
file /out/shairport-sync
'

echo "Built: $OUT"
ls -lh "$OUT"
