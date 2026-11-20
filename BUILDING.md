# Building Homebrew Store PS4

## GitHub Actions (Linux)

[.github/workflows/build-pkg.yml](.github/workflows/build-pkg.yml) builds `HomebrewStore-PS4-*.pkg` on `ubuntu-24.04`:
- Installs `clang-18`, `lld-18`, `python3`, `python3-pil`, `make`, and `libssl1.1` (required by .NET Core 3.1 `PkgTool.Core`).
- Downloads the OpenOrbis PS4 Toolchain (`v0.5.4`, `toolchain-llvm-18.tar.gz`) and copies `libc.prx` / `libSceFios2.prx`.
- Runs `make test` (Web Panel Python tests + C++ host tests).
- Builds `build/IV0000-HBST00001_00-HBSTORE000000000.pkg` and validates it with `PkgTool.Core pkg_validate` and `tools/verify_package.py`.
- Uploads the PKG as a workflow artifact, or publishes a GitHub Release when pushing a `v*` tag.

## Local Build (Linux / macOS)

Requirements:
- Clang/LLD (LLVM 18+)
- Python 3 + Pillow
- [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) (`OO_PS4_TOOLCHAIN` env var)
- `sce_module/libc.prx` and `sce_module/libSceFios2.prx` copied from the toolchain

Commands:
```bash
make test
make
make validate
```

To customize the default server URL baked into the PKG before building, edit `assets/server.txt` (default: `http://192.168.1.2:8765`). The server URL can also be changed at runtime inside the PS4 app using **L3**.
