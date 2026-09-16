# CLAUDE.md

Project-specific guidance for working in this repository.

## Changelog policy

`CHANGELOG.md` tracks notable changes to the tool suite. Update it as
part of any commit that:

- adds a new tool,
- adds or removes a user-facing option/flag on an existing tool,
- changes a tool's default behaviour, exit codes, or output format,
- bumps a tool's version macro (`P3M_*_VERSION`),
- fixes a bug a user could have observed.

Add the new entry at the **top** of the file (entries are newest
first), grouped under today's date as a `## YYYY-MM-DD` heading (reuse
the heading if one already exists for today). Follow the existing
entry shape: a `### ` heading naming the tool(s) and a short summary of
the change, a line noting any version bump (` `p3m-x` → 1.2.0` or
` `p3m-x` 1.0.0 (new)` ), then a short paragraph on what changed and,
where it matters, why — mirror the level of detail already in the
file rather than just restating the commit subject line.

Skip the changelog for internal refactors with no user-visible effect,
doc-only fixes, and test/scratch changes — use judgement the same way
you would deciding whether something belongs in a commit message body.
