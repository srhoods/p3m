#!/bin/sh
# Emit the RPM spec on stdout: p3m.spec.in with @VERSION@ taken from VERSION
# and %changelog generated from the release headings in CHANGELOG.md.
# Run from the repository root.
set -eu

version=$(tr -d '[:space:]' < VERSION)
who=$(git config user.name 2>/dev/null || echo "p3m maintainers")
mail=$(git config user.email 2>/dev/null || echo "noreply@example.invalid")

changelog() {
    # '## [1.0.0] - 2026-10-02'  ->  '* Fri Oct 02 2026 Name <mail> - 1.0.0-1'
    sed -n 's/^## \[\([0-9][0-9.]*\)\] - \([0-9-]*\)$/\1 \2/p' CHANGELOG.md |
    while read -r ver date; do
        printf '* %s %s <%s> - %s-1\n' \
            "$(LC_ALL=C date -d "$date" '+%a %b %d %Y')" "$who" "$mail" "$ver"
        printf -- '- Release %s; see CHANGELOG.md for details\n\n' "$ver"
    done
}

sed -e "s/@VERSION@/$version/g" packaging/p3m.spec.in
changelog
