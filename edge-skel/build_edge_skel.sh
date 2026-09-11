#!/bin/bash
# ============================================================
# 5gipc-rc 方案2骨架: ARMv7 交叉编译 libdatachannel + edge_skel
# 目标: OpenIPC ssc338q(4.9.84, 91MB RAM) 内存基线验证
# 产物: dist/edge_skel (静态, 链接 libdatachannel + mbedtls + usrsctp + libsrtp)
# ============================================================
set -euo pipefail

DIST="$(pwd)/dist"
WORK="$(pwd)/work"
TC_DIR="$WORK/toolchain"
STAGE="$WORK/stage"

mkdir -p "$DIST" "$WORK" "$TC_DIR" "$STAGE"

TOOLCHAIN_URL="https://github.com/openipc/firmware/releases/download/toolchain/toolchain.sigmastar-infinity6e.tgz"
MBEDTLS_VER="v2.28.8"
MBEDTLS_URL="https://github.com/Mbed-TLS/mbedtls/archive/refs/tags/${MBEDTLS_VER}.tar.gz"
LIBDC_TAG="v0.24.5"

echo "==> 产物目录: $DIST"

# ---------------- 1. 工具链 ----------------
if [ ! -x "$TC_DIR/bin/arm-openipc-linux-gnueabihf-gcc" ]; then
  echo "==> 下载 OpenIPC 工具链 ..."
  wget -q "$TOOLCHAIN_URL" -O "$WORK/tc.tgz"
  tar -xzf "$WORK/tc.tgz" -C "$TC_DIR" --strip-components=1
fi
export PATH="/usr/bin:/bin:$TC_DIR/bin:$PATH"
CROSS="arm-openipc-linux-gnueabihf-"
"$CROSS"gcc --version | head -1

# ---------------- 2. CMake 交叉编译 toolchain file ----------------
TC_FILE="$WORK/arm.toolchain.cmake"
cat > "$TC_FILE" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_CXX_COMPILER ${CROSS}g++)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH "$STAGE")
EOF

# ---------------- 3. mbedtls(静态, DTLS 后端, 比 OpenSSL 小) ----------------
if [ ! -f "$STAGE/lib/libmbedtls.a" ]; then
  echo "==> 编译 mbedtls $MBEDTLS_VER ..."
  cd "$WORK"
  wget -q "$MBEDTLS_URL" -O mbedtls.tgz
  tar -xzf mbedtls.tgz
  cd mbedtls-${MBEDTLS_VER#v}
  cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
    -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
    -DCMAKE_INSTALL_PREFIX="$STAGE" >/dev/null
  cmake --build build -j"$(nproc)" >/dev/null
  cmake --install build >/dev/null
fi

# ---------------- 4. libdatachannel(静态) ----------------
if [ ! -f "$WORK/libdatachannel/build/libdatachannel.a" ]; then
  echo "==> clone libdatachannel $LIBDC_TAG ..."
  if [ ! -d "$WORK/libdatachannel" ]; then
    git clone --depth 1 --branch "$LIBDC_TAG" --recurse-submodules \
      https://github.com/paullouisageneau/libdatachannel.git "$WORK/libdatachannel"
  fi
  echo "==> 交叉编译 libdatachannel ..."
  cd "$WORK/libdatachannel"
  cmake -B build \
    -DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
    -DCMAKE_PREFIX_PATH="$STAGE" \
    -DBUILD_SHARED_LIBS=OFF \
    -DUSE_MBEDTLS=ON \
    -DNO_EXAMPLES=ON -DNO_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j"$(nproc)"
fi

# ---------------- 5. 编译 edge_skel(静态) ----------------
echo "==> 编译 edge_skel ..."
cd "$(dirname "$0")"
LDC="$WORK/libdatachannel"
# 收集 libdatachannel 构建的静态库(submodule 产物路径不固定, 用 find)
SRTP=$(find "$LDC/build" -name "libsrtp2.a" | head -1)
SCTP=$(find "$LDC/build" -name "libusrsctp.a" | head -1)
"$CROSS"g++ -std=c++17 -O2 -static \
  -I"$LDC/include" -I"$LDC/deps/json/include" \
  edge_skel.cpp \
  "$LDC/build/libdatachannel.a" "$SRTP" "$SCTP" \
  "$STAGE/lib/libmbedtls.a" "$STAGE/lib/libmbedx509.a" "$STAGE/lib/libmbedcrypto.a" \
  -lpthread -ldl -lm \
  -o "$DIST/edge_skel"

echo "==> 产物:"
file "$DIST/edge_skel"
ls -la "$DIST/edge_skel"
echo "完成。传到设备: /tmp/edge_skel && /tmp/edge_skel 60 (打印 VmRSS/VmPeak)"
