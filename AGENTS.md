# Application integration

Develop against a matched SrvrOS checkout and its pinned SrvrUI revision. Read
SrvrOS `AGENTS.md` and `docs/agent-coordination.md` before shared changes.
Keep app behavior in this repository; shared controls belong in SrvrUI and
transport, services and system policy belong in SrvrOS. Coordinate changes to
shared interfaces across the affected repositories and test the combined pinned
revisions. Run builds and desktop tests from the SrvrOS root; do not run
concurrent builds or VM harnesses in one checkout because they share outputs.
Record the exact revisions and relevant host/VM results with the change.
