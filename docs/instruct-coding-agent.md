# Instructing A Coding Agent To Build A Test Client

This page is for the operator who already has the FreeBSD SCTP conformance
server running and wants to point a coding agent at one client codebase such as
`clients/go-sctp` or `clients/rust-sctp`.

The goal is to give the agent enough context to build a real SCTP client
against the server without teaching it server internals that it should not own.

## What To Give The Agent

Give the agent these inputs:

- the conformance server base URL, for example `http://free.metatao.net:18080`
- the client repository or submodule it should modify, for example `clients/go-sctp`
- the rule that the FreeBSD server is the reference implementation and owns the feature contracts
- the rule that the client must use `GET /v1/features` to discover the catalog
- the rule that the client must call `POST /v1/sessions/{sessionId}/features/{featureId}/start` and consume the returned `contract`
- the rule that unsupported features must be reported explicitly with evidence instead of skipped

Also tell the agent which runtime it is working in:

- Go: `clients/go-sctp`
- Rust: `clients/rust-sctp`

## Recommended Operator Prompt

Use a prompt shaped like this:

```text
Build an SCTP conformance client in `clients/go-sctp`.

Target server:
- Base URL: http://free.metatao.net:18080

Rules:
- The FreeBSD SCTP conformance server is the reference implementation.
- Do not hardcode per-feature payloads or peer addresses.
- Fetch the feature catalog from `GET /v1/features`.
- Create a session with `POST /v1/sessions`.
- For each feature, call `POST /v1/sessions/{sessionId}/features/{featureId}/start`.
- Read the returned `contract` and use its fields as the authoritative instructions for that feature.
- Run features serially because the server allows one active feature per session.
- If a feature requires agent-side reporting, call `complete`.
- If a feature is not possible in the current runtime, call `unsupported` with concrete evidence.
- Do not guess. Use the server contract and the runtime API that actually exists in this repository.

Deliverables:
- the client code changes
- build and test commands
- the final session id and dashboard URL
- a summary of any unsupported features with reasons
```

Swap `clients/go-sctp` for `clients/rust-sctp` or another client codebase as needed.

## Expected Agent Workflow

The expected feature loop is:

1. `GET /v1/features`
2. `POST /v1/sessions`
3. For one feature:
   - `POST /v1/sessions/{sessionId}/features/{featureId}/start`
   - read `contract`
   - execute the SCTP scenario described by that contract
   - `GET /v1/sessions/{sessionId}/features/{featureId}` or watch the summary stream
   - if required, `POST /complete` or `POST /unsupported`
4. `GET /v1/sessions/{sessionId}/summary`

Important:

- the server owns `connect_addresses`, `client_send_messages`, `server_send_messages`, socket-option requirements, and reporting rules
- the client should stay generic and consume those contract fields
- the dashboard and summary endpoints are for visibility, not as a substitute for the contract

## What “Good” Instructions Look Like

Good instructions tell the agent:

- where it is allowed to edit code
- where the server lives
- which API endpoints are authoritative
- how unsupported features should be reported
- what outputs you expect at the end

Bad instructions tell the agent:

- to reverse-engineer payloads from old runs
- to hardcode SCTP addresses or feature-specific messages
- to mark features as passed without talking to the server
- to skip unsupported features silently

## Go-Specific Note

The published Go client already has a generic scenario catalog in:

- `clients/go-sctp/misc/sctp-feature-client/go/scenarios.go`

That catalog is keyed by the same `feature_id` values returned by the server.
The Go client also has:

- `--list-scenarios`

which prints the local `feature_id -> implementation` mapping without making any
server requests.

## Useful References

- [Agent HTTP API](/home/olivier/Projects/sctp/sctp-conformance/docs/agent-http-api.md)
- [Main README](/home/olivier/Projects/sctp/sctp-conformance/README.md)
