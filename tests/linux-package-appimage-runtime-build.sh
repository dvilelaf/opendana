#!/usr/bin/env bash
# C6.50: reproducibility of the AppImage Type-2 runtime.
#   bash tests/linux-package-appimage-runtime-build.sh [SOURCES_DIR]
# Builds the runtime twice from clean (two fresh containers, packaging/appimage-runtime/build.sh
# --twice) and requires (a) the two SHA-256 to be equal and (b) equal to the pin in
# packaging/pins.sh; then runs the structural guard on it and a positive control: the same
# check against a deliberately wrong pin must fail. Optionally (OD_RT_FROM_OFFER=<offer.tar.xz>)
# rebuilds from the published source archive and requires the same hash.
# SOURCES_DIR defaults to build/appimage-runtime/sources (upstream tarballs of sources.lock;
# OD_RUNTIME_FETCH=1 downloads the missing ones). Exit 2 (BLOCKED) without docker.
set -euo pipefail
cd "$(dirname "$0")/.."
source packaging/pins.sh
command -v docker >/dev/null || { echo 'BLOCKED: docker required' >&2; exit 2; }
sources="${1:-build/appimage-runtime/sources}"
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-runtime-build.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fail() { echo "error: linux-package-appimage-runtime-build: $*" >&2; exit 1; }
fetch=()
[ "${OD_RUNTIME_FETCH:-0}" != 1 ] || fetch=(--fetch)
bash packaging/appimage-runtime/build.sh --sources "$sources" --out "$work/out" --twice "${fetch[@]}" | tee "$work/build.log"
grep -q '^reproducible: ' "$work/build.log" || fail 'the two clean builds were not reported identical'
hash="$(sha256sum "$work/out/runtime-x86_64" | cut -d' ' -f1)"
[ "$hash" = "$OD_PIN_APPIMAGE_RUNTIME_SHA256" ] \
    || fail "built runtime $hash is not the pinned $OD_PIN_APPIMAGE_RUNTIME_SHA256 (see packaging/pins.sh)"
bash packaging/appimage-runtime-check.sh "$work/out/runtime-x86_64" "$OD_PIN_APPIMAGE_RUNTIME_SHA256" > /dev/null
# positive control: the gate must reject a runtime that does not match the pin
if bash packaging/appimage-runtime-check.sh "$work/out/runtime-x86_64" "$(printf '0%.0s' $(seq 64))" 2> "$work/neg.log"; then
    fail 'positive control failed: a wrong pin was accepted'
fi
grep -q 'SHA-256 mismatch' "$work/neg.log" || fail 'positive control: unexpected rejection reason'
if [ -n "${OD_RT_FROM_OFFER:-}" ]; then
    mkdir "$work/offer"
    tar -xf "$OD_RT_FROM_OFFER" -C "$work/offer" --strip-components=1
    bash "$work/offer/build.sh" --sources "$work/offer/sources" --out "$work/offer-out" > "$work/offer.log" 2>&1 \
        || { tail -20 "$work/offer.log" >&2; fail 'build from the source archive failed'; }
    [ "$(sha256sum "$work/offer-out/runtime-x86_64" | cut -d' ' -f1)" = "$hash" ] \
        || fail 'the runtime built from the published source archive differs'
    echo "source archive: same runtime"
fi
echo "linux-package-appimage-runtime-build: PASS (two clean builds -> $hash, equal to the pin)"
