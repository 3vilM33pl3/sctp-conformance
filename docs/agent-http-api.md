# Agent HTTP API

The FreeBSD reference server exposes a small HTTP/JSON control plane.

Health check:

- `GET /healthz`

## Session Flow

1. `GET /v1/features`
2. `POST /v1/sessions`
3. `POST /v1/sessions/{sessionId}/features/{featureId}/start`
4. Execute the SCTP scenario in the client environment
5. Poll `GET /v1/sessions/{sessionId}/features/{featureId}`
6. If required by the feature's `completion_mode`, call:
   - `POST /v1/sessions/{sessionId}/features/{featureId}/complete`
   - or `POST /v1/sessions/{sessionId}/features/{featureId}/unsupported`
7. Fetch `GET /v1/sessions/{sessionId}/summary`

## Endpoints

### `GET /v1/features`

Returns the feature catalog and the completion mode for each feature:

- `server_observed`: the server marks pass/fail based on SCTP behavior
- `agent_reported`: the client must report successful local execution
- `hybrid`: the server must observe SCTP behavior and the client must report local success

The catalog is the authoritative list of features an agent should attempt.

### `POST /v1/sessions`

Optional flat JSON body:

```json
{
  "agent_name": "codex-cli",
  "environment_name": "go-sctp-linux"
}
```

### `POST /v1/sessions/{sessionId}/features/{featureId}/start`

Starts one feature in the session and returns a scenario contract containing:

- SCTP addresses to connect to
- an invalid target for negative-path features
- required client socket options
- required subscriptions
- required messages to send
- optional messages the server will send
- timeout and completion mode
- reporting instructions

### `GET /v1/sessions/{sessionId}/features/{featureId}`

Returns the per-feature state:

- `pending`
- `active`
- `passed`
- `failed`
- `unsupported`
- `timed_out`

### `POST /v1/sessions/{sessionId}/features/{featureId}/complete`

Flat JSON body:

```json
{
  "evidence_kind": "runtime",
  "evidence_text": "Set SCTP_NODELAY succeeded",
  "report_text": "Observed association and shutdown notifications"
}
```

For `agent_reported` and `hybrid` features, this marks the agent side of the scenario as complete.

### `POST /v1/sessions/{sessionId}/features/{featureId}/unsupported`

Flat JSON body:

```json
{
  "reason": "missing API",
  "evidence_kind": "compile_error",
  "evidence_text": "SCTP_DEFAULT_SNDINFO is not exposed in this runtime"
}
```

## Notes

- The server allows one active feature at a time per session.
- The server never performs root actions.
- Features that need additional local FreeBSD setup fail with explicit manual commands.
- Active SCTP sockets are closed immediately when a feature becomes `failed`, `unsupported`, or `timed_out`.
