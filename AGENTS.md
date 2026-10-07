# Repository Guidelines

## Project Structure & Module Organization

QEMU Camp 2026 contains RISC-V CPU, SoC, GPGPU, and Rust experiments.
Instruction emulation lives in `target/riscv/`; the G233 board is
`hw/riscv/g233.c`, with device models under `hw/` and headers under `include/`.
GPGPU: `hw/gpgpu/`; Rust devices: `rust/hw/`.

Camp tests live in `tests/gevico/tcg/` and `tests/gevico/qtest/`; GPGPU tests
are in `tests/qtest/gpgpu-test.c`. Other tests include `tests/unit/`
and `tests/functional/`. Read `README.md`, `README_zh.md`, and `docs/devel/`
for setup and internals. Firmware lives in `pc-bios/`; build output belongs in `build/`.

## Build, Test, and Development Commands

Run from the repository root. Install QEMU build dependencies, a RISC-V
bare-metal compiler (`riscv64-unknown-elf-gcc`), Rust, and `bindgen-cli`
following `README.md`.

- `make -f Makefile.camp configure`: configure RISC-V system/user emulation
  and Rust support in `build/`.
- `make -f Makefile.camp build JOBS=4`: compile using four parallel jobs.
- `make -f Makefile.camp test-cpu`: run instruction tests through TCG.
- `make -f Makefile.camp test-soc`: run G233 peripheral QTests.
- `make -f Makefile.camp test-gpgpu`: run GPGPU QOS subtests.
- `make -f Makefile.camp test-rust`: run Rust unit and device QTests.
- `make -f Makefile.camp test`: run all camp suites.
- `build/qemu-system-riscv64 -machine help`: list available machines.

## Coding Style & Naming Conventions

Follow `.editorconfig` and `docs/devel/style.rst`: four-space C indentation,
tabs for Makefile recipes, LF endings, and lines preferably within 80 columns.
Use `snake_case` functions/variables, `CamelCase` types, and uppercase macros.
Include `qemu/osdep.h` first in C sources. Check the latest commit with
`scripts/checkpatch.pl --branch HEAD^..HEAD`. Format Rust with
`make -C build rustfmt` (nightly rustfmt); follow `docs/devel/rust.rst` for
Clippy through Meson's development environment.

## Testing Guidelines

Use TCG bare-metal tests, GLib-based QTest/QOS tests, and native Rust unit
tests. Name instruction tests `test-insn-*.c`, peripheral tests `test-*.c`,
and Rust test functions `test_*`. Register new tests in the relevant Meson
or Makefile lists. Cover changed behavior and regression cases; no camp
coverage percentage is specified. Run the affected suite and inspect pass
counts and logs: scoring wrappers and CI can succeed despite failed tests.

## Commit & Pull Request Guidelines

History mixes plain summaries with `ci:` and `tests/gevico:` prefixes.
Prefer `subsystem: imperative summary`, such as `tests/gevico: fix SPI chip select`.
Explain why; add `Signed-off-by` for upstream submissions.
PRs should describe behavior, link relevant issues,
and report test commands and pass counts. Camp CI runs on eligible pushes
to `main`; verify locally before requesting review.

## Agent-Specific Instructions

Explain concepts in prose. Avoid comparison tables, especially
responsibility tables or field mappings that replace conceptual explanations.

When explaining QEMU concepts or working on camp exercises, consult the
reference materials in the repository's parent directory. Resolve these
paths relative to the repository root:

- `../00_QEMU GPGPU 四阶段实验大纲.md`
- `../QEMU_2026_GPGPU_适配.html`
- `../QEMU_2026_实验.html`
- `../QEMU_2026_讲义.html`

If the `00_QEMU` outline is absent, use `../QEMU GPGPU 实验大纲.md`.

## Current GPGPU Experiment

Advanced experiment one is complete on branch `hxfei-qemu`. The stage-one
software stack lives in `software/gpgpu/`: a Linux PCI driver, libgpgpu,
RV32 assembly kernels and frontend, three FP32 demos, and guest integration
tests. Read `software/gpgpu/README.md` and `WORK_ITEMS.md` before changing
its interfaces. The current ABI is `software/gpgpu/include/gpgpu_uapi.h`.

Run `make -f Makefile.camp test-gpgpu-stack` for the ARM64 Linux driver and
runtime tests. First use bootstraps Alpine; later builds reuse the compiler
disk under `build/gpgpu-linux-module/`. Run
`make -C software/gpgpu check-assembler` for frontend encoding tests.

Stage-one validation on 2026-10-07 passed three demos (17,953 output values),
68 driver/runtime checks and 21 GPGPU QTests on both RISC-V and ARM64 virt.
The 21 tests include the 17 scoring tests and four regressions. Inspect test
counts and logs; do not infer success from the scoring wrapper exit status.

The device uses exclusive sessions and synchronous calls. Device pointers
are VRAM offsets; mmap maps the 64 KiB coherent DMA staging buffer.
Shared memory/barriers, CP queues, async streams and GPU address isolation
are future work. The interpreter supports BF16, FP8 and FP4 conversions;
it does not implement FP16 conversion. Migration is explicitly disabled.
