#!/usr/bin/env bash
# C6.30: CMake controls (configure only; no SDL, no build).
#  1. a `git archive` of HEAD configures with the stamped commit as --version;
#  2. -DOD_GIT_COMMIT overrides it;
#  3. negative control: the same tree with an unstamped source-commit.txt and
#     no git says "commit unknown" (the stamp, not luck, carries the commit);
#  4. the source archive (no tests/) configures with the test runner disabled;
#  5. OD_PACKAGE_ASSETS / OD_INSTALL_ASSETS are refused (cannot ship assets);
#  6. OD_PACKAGE=ON on Linux generates no CPack configuration (TGZ retired);
#  7. OD_BUILD_PRIVATE_TESTS=ON without OD_PRIVATE_TEST_VIEW is refused (the
#     comparators would read data that left the public tree, C6.27).
set -euo pipefail
cd "$(dirname "$0")/.."
work="$(mktemp -d "${TMPDIR:-/tmp}/opendana-cmake-test.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fail() { echo "error: linux-package-cmake: $*" >&2; exit 1; }
commit="$(git rev-parse HEAD)"
mapfile -t paths < <(sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' packaging/application-source.list)
git archive --format=tar --prefix=src/ HEAD -- "${paths[@]}" | tar -xf - -C "$work"
configure() { cmake -S "$work/src" -B "$work/$1" -DOD_BUILD_APP=OFF "${@:2}" 2>&1; }
out="$(configure b1)"
grep -q "opendana --version: .* (commit $commit)" <<<"$out" || fail "stamped commit not used"
grep -q 'tests/ not present (source archive)' <<<"$out" || fail 'test runner not disabled without tests/'
echo "stamped archive: --version commit $commit; test runner disabled"
out="$(configure b2 -DOD_GIT_COMMIT=0123456789abcdef0123456789abcdef01234567)"
grep -q '(commit 0123456789abcdef0123456789abcdef01234567)' <<<"$out" || fail 'override ignored'
echo "override: -DOD_GIT_COMMIT honoured"
printf '%s\n' '$Format:%H$' > "$work/src/packaging/source-commit.txt"
out="$(GIT_CEILING_DIRECTORIES="$work" configure b3)"
grep -q '(commit unknown)' <<<"$out" || fail "negative control: unstamped tree did not say unknown: $(grep 'opendana --version' <<<"$out")"
echo "negative control: unstamped tree outside git -> commit unknown"
for opt in OD_PACKAGE_ASSETS OD_INSTALL_ASSETS; do
    if configure "b-$opt" "-D$opt=ON" > "$work/$opt.log"; then fail "$opt=ON accepted"; fi
    grep -q "$opt was removed" "$work/$opt.log" || fail "$opt refused without explanation"
done
echo "OD_PACKAGE_ASSETS / OD_INSTALL_ASSETS refused"
configure b4 -DOD_PACKAGE=ON > /dev/null
[ ! -e "$work/b4/CPackConfig.cmake" ] || fail 'CPack still configured on Linux'
echo "OD_PACKAGE=ON on Linux: no CPackConfig.cmake"
if cmake -S . -B "$work/b5" -DOD_BUILD_APP=OFF -DOD_BUILD_PRIVATE_TESTS=ON > "$work/b5.log" 2>&1; then
    fail 'OD_BUILD_PRIVATE_TESTS=ON accepted without a private view'
fi
grep -q 'OD_PRIVATE_TEST_VIEW' "$work/b5.log" || fail 'private tests refused without explanation'
echo "OD_BUILD_PRIVATE_TESTS=ON without OD_PRIVATE_TEST_VIEW refused"
echo 'linux-package-cmake: PASS'
