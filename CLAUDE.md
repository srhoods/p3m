# CLAUDE.md

Project-specific guidance for working in this repository.

## Changelog policy

`CHANGELOG.md` tracks notable changes to the tool suite. Update it as
part of any commit that:

- adds a new tool,
- adds or removes a user-facing option/flag on an existing tool,
- changes a tool's default behaviour, exit codes, or output format,
- fixes a bug a user could have observed.

Add the entry under `## [Unreleased]` at the top of the file, in a
Keep a Changelog group (`### Added`, `### Changed`, `### Fixed`,
`### Removed`), naming the tool(s) affected and saying what changed
and, where it matters, why — mirror the level of detail of the 1.0.0
entry rather than just restating the commit subject line.

## Versioning and releases

The suite is versioned as a whole with Semantic Versioning; there are
no per-tool version numbers. `VERSION` is the single source of truth
(compiled into every binary via `-DP3M_VERSION`, used by the RPM spec
and the man pages). Do not hard-code a version anywhere else.

To release: move the `[Unreleased]` entries under a new
`## [x.y.z] - YYYY-MM-DD` heading in `CHANGELOG.md` (leave an empty
`[Unreleased]` above it), set `VERSION` to `x.y.z`, run
`make check-version rpm`, commit, and tag `vx.y.z`. Bump MAJOR for
incompatible CLI/output/exit-code changes, MINOR for new tools or
options, PATCH for bug fixes. `make check-version` must pass.

New tools also need a man page in `man/` (`NAME.1.in`, using the
`@VERSION@`/`@DATE@` placeholders) and an entry in `NAMES` in the
`Makefile`, `packaging/p3m.spec.in`'s description, and `man/p3m.7.in`.

Skip the changelog for internal refactors with no user-visible effect,
doc-only fixes, and test/scratch changes — use judgement the same way
you would deciding whether something belongs in a commit message body.
