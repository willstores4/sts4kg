# Building NULL_store

The application, optional service, torrent engine, installer adapter and host tests are in this repository. The packages on the Releases page are prebuilt from this source, with additional generated assets and tools described below.

## Requirements

The provided Makefile targets macOS with Apple command-line tools, Python 3, Pillow, Homebrew LLVM/LLD, and the [OpenOrbis PS4 toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain). Set `OO_PS4_TOOLCHAIN` to the toolchain directory and `LLVM` to the LLVM binary directory if they differ from the Makefile defaults. Packaging also needs LibOrbisPkg through the toolchain.

## Assets and package build

Generated packages, console runtime modules, the compatibility helper, cover images and the generated cover archive are not included in the source tree. The package targets require additional assets, including:

- `sce_module/libc.prx` and `sce_module/libSceFios2.prx`, from a compatible OpenOrbis toolchain setup.
- `assets/service/compat-jb.prx`, the Itemzflow compatibility module linked in the README, and `assets/service/COMPAT-PROVENANCE.txt`. Follow the upstream license and retain its notices when redistributing it.
- `assets/covers/*.jpg`, then `make assets/covers.pack`. Cover images can be collected for the bundled catalogue with `mkdir -p assets/covers && python3 tools/scrape_sources.py --covers-only`; third-party sites can be unavailable or change their pages. Respect site terms and artwork rights.

After supplying these inputs, run `make` for the store and `make service-package` for the setup app. The Makefile also generates the service executable and package metadata. Rebuilding has not been verified from a fresh toolchain checkout; missing tools or compatible runtime modules may need additional setup.

## GitHub Actions (Linux)

`.github/workflows/build-pkg.yml` builds both packages on `ubuntu-24.04`: it installs clang/lld 18, downloads the OpenOrbis toolchain (`v0.5.4`, `toolchain-llvm-18.tar.gz`), copies `libc.prx`/`libSceFios2.prx` from the toolchain, fetches the pinned Itemzflow `jb.prx` (SHA-256 checked) and runs `make` + `make service-package`. The PKGs are uploaded as workflow artifacts; pushing a `v*` tag (e.g. `v1.0`) also publishes them to a GitHub Release. Covers committed under `assets/covers/*.jpg` are packed; otherwise the cover archive is empty. The Makefile auto-detects macOS/Linux; override `LLVM`, `PS4_LD`, `CREATE_FSELF` or `PKGTOOL` if needed.

## Host checks

`make test` runs the host suite on macOS using the OpenOrbis headers. Some UI tests also require the cover archive and individual covers. `make preview` creates a software-rendered catalogue image in `build/preview.png`. These checks do not replace testing on a PS4.

The certificate bundle, SDK notices and installer provenance retain their respective notices under `assets/`. The Itemzflow helper is a separate upstream component and its source is linked in the README.
