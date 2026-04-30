# Contributing

## Rules

- **Single-header**: All logic in `vibe.h` under `VIBE_IMPLEMENTATION`.
- **Zero-dep**: C11, `pthread`, and Linux syscalls only.
- **Performance**: Lock-free producers, non-blocking consumer. Use the internal pool.
- **Style**: 2-space indent. GCC/Clang.

## Workflow

1. Fork and branch from `main`.
2. `make` to verify examples.
3. Open a PR with a concise description.

## License

Apache 2.0. New files must include the license header.
