# Contributing

Thanks for contributing to smart_nic! Small, focused PRs are preferred.

## Development Setup

```bash
git clone --recursive https://github.com/rosslwheeler/smart_nic.git
cd smart_nic
make all          # configure + build (Debug)
make test-notrace # run tests without Tracy capture
```

## Code Style

This project follows C++20 coding standards documented in `docs/coding-standards.md`.
Formatting is enforced by `.clang-format` (Google-based, 2-space indent, 100-column limit).

Key conventions:
- `PascalCase` for types, `snake_case` for functions/variables, `kCamelCase` for constants
- Always use braces for `if` statements
- No ternary operators
- Functions under 100 lines
- Every function must include `NIC_TRACE_SCOPED(__func__);`

Format your code before submitting:
```bash
find include src tests driver -name '*.h' -o -name '*.cpp' | xargs clang-format -i
```

## Testing

- Add or update tests when changing behavior
- Run the full test suite before submitting: `make test-notrace`
- Run with AddressSanitizer: `make asan`
- Check code coverage: `make coverage`

## Pull Requests

- Keep changes scoped and explain the motivation in the PR description
- Ensure all tests pass and formatting is clean
- New features should include tests
