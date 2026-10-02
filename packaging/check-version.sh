#!/bin/sh
# Verify that VERSION, the newest CHANGELOG.md release heading and the RPM
# spec template agree. Run from the repository root (`make check-version`).
set -eu

fail() { echo "check-version: $*" >&2; exit 1; }

[ -f VERSION ] || fail "VERSION file missing"
version=$(tr -d '[:space:]' < VERSION)
echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' ||
    fail "VERSION '$version' is not MAJOR.MINOR.PATCH"

top=$(sed -n 's/^## \[\([0-9][0-9.]*\)\] - .*/\1/p' CHANGELOG.md | head -1)
[ -n "$top" ] || fail "CHANGELOG.md has no '## [x.y.z] - DATE' release heading"
[ "$top" = "$version" ] ||
    fail "VERSION is $version but the newest CHANGELOG.md release is $top"

grep -q '^## \[Unreleased\]' CHANGELOG.md ||
    fail "CHANGELOG.md is missing its '## [Unreleased]' section"

grep -q '^Version:[[:space:]]*@VERSION@' packaging/p3m.spec.in ||
    fail "packaging/p3m.spec.in must use 'Version: @VERSION@' (no hard-coded version)"

echo "check-version: OK ($version)"
