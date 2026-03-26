# SCTP Conformance Suite

The primary interface is now a FreeBSD-resident SCTP feature server.

The server runs against the FreeBSD reference implementation and exposes:

- an HTTP/JSON control plane for feature discovery, session management, and result reporting
- an SCTP data plane where the actual transport scenarios execute

The intended workflow is:

1. Run `sctp-feature-server` on a FreeBSD host.
2. Give a coding agent the server URL, the feature catalog, and a client codebase such as `go-sctp-linux` or `rust-sctp`.
3. Let the agent build a client that attempts each feature in turn.
4. The server marks features `passed` or `failed` from observed SCTP behavior, and the agent can declare a feature `unsupported` with evidence.

The older Python runner is still present for parity work, but it is now secondary.

## Layout

- `server/freebsd_ref/`: FreeBSD C++ reference server
- `docs/agent-http-api.md`: HTTP control plane used by coding agents
- `scripts/smoke_feature_server.py`: acceptance smoke test for the server API and SCTP data plane
- `adapters/freebsd_c/`: FreeBSD C helper used for smoke testing and baseline validation
- `runner/`, `profiles/`, `scenarios/`: legacy runner-centric harness kept for migration and comparison work

## FreeBSD Server

Build on the FreeBSD host with base tools:

```sh
cd server/freebsd_ref
make
```

Run the server:

```sh
./sctp-feature-server \
  --http-host 0.0.0.0 \
  --http-port 18080 \
  --sctp-addrs 10.22.6.90,127.0.0.2 \
  --advertise-addrs 10.22.6.90,127.0.0.2
```

- `--sctp-addrs` are the local FreeBSD addresses the server binds.
- `--advertise-addrs` are the addresses returned in feature contracts.
- One active feature is allowed per session.
- All state is in memory only.

## Manual Prerequisites

The server never performs root actions.

If SCTP is not loaded or a required local address is missing, the server or feature activation fails with the exact command to run manually.

Current prerequisites on the FreeBSD host:

```sh
kldload /boot/kernel/sctp.ko
ifconfig lo0 alias 127.0.0.2/8
```

If you want remote multihoming scenarios to use non-loopback addresses, add those addresses to a real interface manually and start the server with matching `--sctp-addrs` and `--advertise-addrs`.

## Agent Flow

The control plane is documented in [docs/agent-http-api.md](/home/olivier/Projects/sctp/sctp-conformance/docs/agent-http-api.md).

Minimal flow:

```text
GET  /v1/features
POST /v1/sessions
POST /v1/sessions/{sessionId}/features/{featureId}/start
GET  /v1/sessions/{sessionId}/features/{featureId}
POST /v1/sessions/{sessionId}/features/{featureId}/complete
POST /v1/sessions/{sessionId}/features/{featureId}/unsupported
GET  /v1/sessions/{sessionId}/summary
```

The current server catalog covers:

- socket creation
- basic association setup
- single and multi-message boundaries
- stream ID and PPID metadata
- `SCTP_NODELAY`
- `SCTP_INITMSG`
- `SCTP_RTOINFO`
- notifications
- event subscription coverage and shutdown notifications
- multihome bind/connect and address enumeration
- `SCTP_BINDX` add/remove
- primary-address management requests
- negative connect error handling
- `SCTP_DEFAULT_SNDINFO` and server-side receive metadata checks
- `SCTP_RECVNXTINFO`
- unordered delivery attempts
- `SCTP_AUTOCLOSE`
- association peeloff and association ID enumeration
- association status introspection
- stream reconfiguration reset and add-stream attempts

## Smoke Test

After building the FreeBSD helper on the FreeBSD host:

```sh
cd adapters/freebsd_c
make
```

Run the smoke test from the local machine:

```sh
python3 scripts/smoke_feature_server.py \
  --base-url http://free.metatao.net:18080 \
  --ssh-host free.metatao.net \
  --helper-path /tmp/sctp-freebsd-helper/sctp-freebsd-helper
```

The smoke test exercises:

- one `server_observed` feature
- one `hybrid` feature
- one `agent_reported` feature

## Legacy Runner

The Python runner remains available:

```sh
python3 sctp_conformance.py list-profiles
python3 sctp_conformance.py run --profile go-sctp-linux
python3 sctp_conformance.py run --profile freebsd-oracle
```

It is retained for migration and comparison, not as the main interface for coding agents.
