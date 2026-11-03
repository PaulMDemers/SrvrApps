# SrvrApps

This repository contains the native desktop applications Clock, Find Files,
Help, Network Utility, Sketch, Terminal, the standard desktop tools (including
Files, Text, Settings, System and Calculator), and Preview. The desktop shell,
services, client runtime, operating system and command-line programs remain in
SrvrOS; shared widgets live in SrvrUI.

These applications are built in the SrvrOS source tree against its userspace
SDK, desktop runtime, shared interfaces and SrvrUI. From the SrvrOS root, use
`tools/host-env.sh make -j2 all` to build the system and `make test-desktop`
for host desktop tests. See `docs/native-desktop.md` and
`docs/desktop-application-lifecycle.md` in SrvrOS for integration and lifecycle
verification. Application executable and installed names do not follow the
source directory layout.

## Independent development

This repository is pinned at `userspace/apps/` by
[SrvrOS](https://github.com/PaulMDemers/SrvrOS). Clone the OS recursively, create
a branch inside this submodule, and build from the OS root. On Apple Silicon
use `sh tools/macos/build.sh -j2 all`. Shared widgets come from SrvrUI at
`userspace/widgets/`; apps should extend shared controls there. Commit and push
application changes before updating the OS gitlink. Cross-repository ABI changes
need a matched OS/UI/app revision set and combined tests.

The initial import preserves application bytes from SrvrOS `74d38d59`; earlier
application history remains in that repository under `userspace/desk*`. The
extraction commit groups those files without changing executable names.
