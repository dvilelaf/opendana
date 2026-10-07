#!/bin/bash
# C6.50: runs INSIDE the pinned build container (no network). Builds every library
# linked into the AppImage Type-2 runtime from the source tarballs in /src and then
# the runtime itself, statically, with deterministic flags.
#
#   /src   read-only: the tarballs of sources.lock (verified again here) + sources.lock
#   /out   output: runtime-x86_64 (+ build-info.txt)
#
# Environment: SOURCE_DATE_EPOCH (required).
set -euo pipefail
export LC_ALL=C TZ=UTC
umask 022
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH is required}"

lock=/src/sources.lock
get() { awk -F'\t' -v n="$1" -v c="$2" '$1==n && $0 !~ /^#/ {print $c}' "$lock"; }
unpack() { # name -> /build/<name> (stripped top directory)
    local name="$1" file sha
    file="/src/$(get "$name" 4)"; sha="$(get "$name" 3)"
    [ "$(sha256sum "$file" | cut -d' ' -f1)" = "$sha" ] || { echo "error: SHA-256 mismatch for $file" >&2; exit 1; }
    mkdir -p "/build/$name"
    tar -xf "$file" -C "/build/$name" --strip-components=1
}

rm -rf /build /opt/deps
mkdir -p /build /opt/deps /out
for n in type2-runtime libfuse squashfuse zstd zlib mimalloc; do unpack "$n"; done

# Deterministic flags: no build paths (-ffile-prefix-map), no build-id, fixed
# archive members, sources compiled under the same absolute directory every time.
export CC=clang
export CFLAGS="-Os -ffunction-sections -fdata-sections -fPIC -ffile-prefix-map=/build=. -ffile-prefix-map=/opt/deps=/deps -fdebug-prefix-map=/build=."
export LDFLAGS="-Wl,--build-id=none"
export AR=llvm-ar RANLIB=llvm-ranlib ARFLAGS="rcsD"
export PKG_CONFIG_PATH=/opt/deps/lib/pkgconfig
export PKG_CONFIG_LIBDIR=/opt/deps/lib/pkgconfig
jobs="$(nproc)"

echo "== zlib $(get zlib 2) =="
( cd /build/zlib && ./configure --static --prefix=/opt/deps && make -j"$jobs" && make install )

echo "== zstd $(get zstd 2) =="
( cd /build/zstd/lib && make -j"$jobs" libzstd.a ZSTD_LEGACY_SUPPORT=0 &&
  make install-static install-includes install-pc PREFIX=/opt/deps )

echo "== mimalloc $(get mimalloc 2) =="
cmake -S /build/mimalloc -B /build/mimalloc/b -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_C_FLAGS="$CFLAGS" -DCMAKE_AR=/usr/bin/llvm-ar \
    -DCMAKE_INSTALL_PREFIX=/opt/deps -DMI_BUILD_SHARED=OFF -DMI_BUILD_STATIC=ON -DMI_BUILD_OBJECT=OFF \
    -DMI_BUILD_TESTS=OFF -DMI_INSTALL_TOPLEVEL=ON -DMI_OVERRIDE=ON
cmake --build /build/mimalloc/b -j"$jobs"
cmake --install /build/mimalloc/b

echo "== libfuse $(get libfuse 2) (type2-runtime patch: fusermount from \$FUSERMOUNT_PROG) =="
( cd /build/libfuse && patch -p1 < /build/type2-runtime/patches/libfuse/mount.c.diff &&
  meson setup b --prefix=/opt/deps --libdir=lib --buildtype=minsize -Ddefault_library=static \
      -Dexamples=false -Dtests=false -Dutils=false -Duseroot=false -Db_staticpic=true \
      -Dc_args="$CFLAGS" -Dc_link_args="$LDFLAGS" &&
  ninja -C b && ninja -C b install )

echo "== squashfuse $(get squashfuse 2) =="
( cd /build/squashfuse && ./autogen.sh &&
  ./configure --prefix=/opt/deps --enable-static --disable-shared CPPFLAGS="-I/opt/deps/include" \
      LDFLAGS="-static -L/opt/deps/lib $LDFLAGS" &&
  make -j"$jobs" && make install &&
  mkdir -p /opt/deps/include/squashfuse && /usr/bin/install -c -m 644 ./*.h /opt/deps/include/squashfuse/ )

echo "== type2-runtime $(get type2-runtime 2) =="
cd /build/type2-runtime/src/runtime
echo -n "https://github.com/AppImage/type2-runtime/commit/$(get type2-runtime 2 | cut -c1-7)" > version
export CPATH=/opt/deps/include LIBRARY_PATH=/opt/deps/lib
make runtime CC="clang -ffile-prefix-map=/build=. -Wl,--build-id=none -I/opt/deps/include/squashfuse -I/opt/deps/include/fuse3 -I/opt/deps/include -L/opt/deps/lib"
strip --strip-debug --strip-unneeded runtime
# classic Type-2 magic bytes, after strip
printf 'AI\002' | dd of=runtime bs=1 count=3 seek=8 conv=notrunc 2>/dev/null
cp runtime /out/runtime-x86_64
chmod 755 /out/runtime-x86_64
touch -d "@$SOURCE_DATE_EPOCH" /out/runtime-x86_64
{
    echo "SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH"
    echo "clang: $(clang --version | head -1)"
    echo "linker: $(ld.lld --version | head -1)"
    echo "musl: $(apk info -v musl musl-dev | tr '\n' ' ')"
    sha256sum /out/runtime-x86_64
    file /out/runtime-x86_64
} > /out/build-info.txt
cat /out/build-info.txt
