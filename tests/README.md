# Tests

This directory contains small root-level validation scripts for the `hlquery`
workspace.

The PHP runner includes unit-style helper tests, benchmark fixture/schema and
configuration validation, compiled artifact checks, source-level HTTP route
catalog checks, wrapper/CLI/benchmark checks, and live daemon API contract tests.
Checks that require generated or compiled artifacts skip cleanly until
`./configure` or a build has produced them.

Set `HLQUERY_URL` for a non-default endpoint and `HLQUERY_API_KEY` when
authentication is enabled. Live tests skip cleanly when the daemon is
unavailable; all repository-level tests still run.

- `tests/php/run_all.php`

Run it with:

```bash
php tests/php/run_all.php
```

For the fullest local run, configure and build the project first:

```bash
./configure
make
php tests/php/run_all.php
```
