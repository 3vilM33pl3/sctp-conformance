# SCTP Conformance Suite

Implementation-agnostic SCTP conformance suite for:

- the reference stack on `FreeBSD`
- the local `go-sctp-linux` runtime branch

The suite is intentionally runner-centric:

- a local Python 3.12 runner loads profiles and scenarios
- adapters expose small `server` and `client` helper binaries
- helpers emit NDJSON events
- assertions live in the runner, not in the helpers

## Layout

- `runner/`: Python package and CLI
- `profiles/`: target definitions
- `scenarios/`: shared scenario corpus and required feature map
- `adapters/freebsd_c/`: FreeBSD C helper compiled on the oracle host
- `adapters/go_net/`: Go helper built with `go-sctp-linux`
- `artifacts/`: generated reports, logs, and captures

## Quick Start

List profiles:

```bash
python3 sctp_conformance.py list-profiles
```

List scenarios:

```bash
python3 sctp_conformance.py list-scenarios
```

Run against local `go-sctp-linux`:

```bash
python3 sctp_conformance.py run --profile go-sctp-linux
```

Run against the FreeBSD oracle host:

```bash
export SCTP_ROOT_PASSWORD_FILE=/path/to/root-password.txt
python3 sctp_conformance.py run --profile freebsd-oracle
```

Results are written under `artifacts/runs/<timestamp>/`.

## FreeBSD Bootstrap

The FreeBSD profile is designed to work with a base FreeBSD 15.0 host. The
runner performs a minimal bootstrap:

- load `sctp.ko` if SCTP sockets are unavailable
- add loopback aliases required by multihome scenarios
- stage and build the helper in a scratch directory under `/tmp`

The root password file path is read from `$SCTP_ROOT_PASSWORD_FILE`.

Root is not required for the SCTP scenarios themselves. It is only needed when
the runner has to:

- load `sctp.ko`
- add the configured loopback aliases
- capture `tcpdump` pcaps

If those prerequisites are already in place on the FreeBSD host, the suite can
run without root and will simply skip pcap capture.

## Current v1 Coverage

The initial scenario set covers:

- socket create/bind/listen/connect
- single and multi-message boundary preservation
- stream ID and PPID round-trip
- `SCTP_NODELAY` acceptance
- `SCTP_INITMSG` acceptance on the listening side
- event subscription and notification presence
- multi-address bind/connect and address enumeration
- negative connect-failure handling

Advanced SCTP extensions remain future work.
