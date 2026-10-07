#!/usr/bin/env bash
# Level 1 (unit): the core, the extension layer and the formats, with fixtures
# of their own. It needs nothing of the original and runs in any clone.
source "$(dirname "$0")/lib.sh"

echo "== pure model tests =="
# The comparator contract reads the git history and the lab-only sources; it is
# part of the lab tier and runs only where it is present.
if [ -f tests/public-tests-contract.py ]; then
    python3 tests/public-tests-contract.py --inventory
    # The full contract (every comparator file against its pinned baseline,
    # written retirements, BLOCKED when a baseline commit is missing). It needs
    # the git history; a clone without it reports BLOCKED, never a silent pass.
    python3 tests/public-tests-contract.py
else
    echo "skipped: lab-only (tests/public-tests-contract.py is not in this tree)"
fi
if [ ! -x "$BUILD_DIR/opendana_tests_public" ]; then
    bash tests/public-tests-build.sh
fi
"$BUILD_DIR/opendana_tests_public" --category unit

echo
echo "== example mods derive their CHR (no CHR is versioned) =="
bash tests/mods-chr-contract.sh

echo
echo "== code provenance (no translated machine code, no third-party tree) =="
# The checker and its positive control belong to the maintainers' tree.
if [ -f tools/audit/test_provenance.sh ]; then
    bash tools/audit/test_provenance.sh
else
    echo "skipped: lab-only (tools/audit/test_provenance.sh is not in this tree)"
fi
