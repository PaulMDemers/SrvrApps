# SrvrOS desktop applications

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
