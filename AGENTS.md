# Repository Guidelines

## Project Structure & Module Organization
Core server code lives in `src/` with public headers in `include/`. Main areas are `src/core/`, `src/api/`, `src/search/`, `src/cli/`, and `src/talk/`. Runtime assets and local launch scripts live in `run/`. Operational tooling, SDKs, docs, Docker assets, and helper scripts live under `etc/`. Keep vendor changes isolated to `vendor/` and only touch them when updating a dependency.

## Build, Test, and Development Commands
Run `./configure` once before building. Use `make -j10` to compile the server and tools; on FreeBSD use `gmake -j10`. Start a local node with `./run/hlquery start` or `./run/hlquery start --nofork` for foreground debugging. Verify API behavior from `etc/api/tests/` with `node test_runner.js`, or run a focused check such as `node test_search.js`. For container smoke tests, use `cd etc/docker && docker-compose up -d`.

## Coding Style & Naming Conventions
C++ follows `.clang-format`: LLVM-based, Allman braces, `IndentWidth: 5`, spaces instead of tabs, unsorted includes, and no column limit. Match surrounding file layout before reformatting. Use `snake_case` for files and most functions, `PascalCase` for types, and keep headers paired with their implementation path when adding modules, for example `include/api/searchapi.h` with `src/api/searchapi.cpp`.

## Testing Guidelines
There is no single top-level `make test` target. Contributors should run the smallest relevant checks for their change, plus a full rebuild when touching shared code. API coverage is centered in `etc/api/tests/`; test files are named `test_*.js`, with Rust, Python, Ruby, Perl, Java, and PHP variants alongside them. When changing runtime behavior, include the exact command you used to validate it in the PR.

## Commit & Pull Request Guidelines
Target `unstable` unless a maintainer asks otherwise. Recent history is terse and inconsistent, so prefer clear imperative subjects such as `Fix vector search pagination` or `Update Docker health check`. Keep PRs small, describe behavior changes, link the issue when applicable, and attach logs or screenshots for CLI, docs, or UI changes.

## Packaging & Repo Sync
Packaging work is maintained in `https://github.com/hlquery/package-builder`. Treat `etc/package-builder/` as synced packaging content rather than the primary home, and use `etc/scripts/hl` when coordinating cross-repo updates.
