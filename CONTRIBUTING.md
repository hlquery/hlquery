### Contributing to hlquery

Thanks for contributing to `hlquery`.

This project is under active development. Keep changes focused, document behavior changes clearly, and validate them locally before opening a pull request.

### Before You Start

- Check existing [issues](https://github.com/hlquery/hlquery/issues) before starting new work.
- Open an issue first for large changes, new features, or behavior changes so the approach can be discussed before implementation.
- Keep pull requests small and reviewable. Separate refactors from feature work when possible.
- All contributions are made under the BSD 3-Clause license used by this repository.

### Branching

The repository uses a two-branch model:

- `unstable`: active development branch for ongoing work
- `1.0`: stable release branch

Unless a maintainer asks otherwise, target `unstable` for new contributions.

## Code Style

- Follow the existing code style in the surrounding files.
- C++ formatting is defined in [`.clang-format`](./.clang-format).
- Keep include ordering as written unless the file already uses a different local pattern.
- Avoid unrelated formatting-only changes in functional pull requests.
- Do not modify vendored code under `vendor/` unless the contribution is explicitly about updating or patching that dependency.

