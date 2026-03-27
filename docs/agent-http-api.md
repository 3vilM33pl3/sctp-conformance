# Agent HTTP API

The FreeBSD reference server exposes a small HTTP/JSON control plane plus a built-in session dashboard.

Health check:

- `GET /healthz`
- `GET /` serves the live session index page with links to every in-memory session dashboard

## Session Flow

1. `GET /` to browse active and historical session dashboards
2. `GET /v1/features`
3. `POST /v1/sessions`
4. Optional: open the returned `dashboard_path` in a browser
5. `POST /v1/sessions/{sessionId}/features/{featureId}/start`
6. Execute the SCTP scenario in the client environment
7. Poll `GET /v1/sessions/{sessionId}/features/{featureId}` or watch `GET /v1/sessions/{sessionId}/summary/stream`
8. If required by the feature's `completion_mode`, call:
   - `POST /v1/sessions/{sessionId}/features/{featureId}/complete`
   - or `POST /v1/sessions/{sessionId}/features/{featureId}/unsupported`
9. Fetch `GET /v1/sessions/{sessionId}/summary`

## Endpoints

### `GET /`

Serves the built-in session index page.

The page auto-refreshes and shows:

- active sessions first
- idle or completed sessions below
- direct links to `GET /sessions/{sessionId}/dashboard`

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

The response includes `session_id`, the per-feature session snapshot, and `dashboard_path`.

### `GET /v1/sessions/{sessionId}`

Returns the current session metadata, including:

- `session_id`
- `agent_name`
- `environment_name`
- `created_at`
- `active_feature_id`
- `dashboard_path`
- per-feature state snapshots

### `GET /sessions/{sessionId}/dashboard`

Serves the built-in single-session traffic-light dashboard.

The page bootstraps from `GET /v1/sessions/{sessionId}` and `GET /v1/sessions/{sessionId}/summary`, then subscribes to `GET /v1/sessions/{sessionId}/summary/stream`.

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

### `GET /v1/sessions/{sessionId}/summary`

Returns the aggregate session state:

- `passed`
- `failed`
- `unsupported`
- `timed_out`
- `pending`
- `active`
- `pending_or_active`
- `complete`
- `features`

### `GET /v1/sessions/{sessionId}/summary/stream`

Server-Sent Events endpoint that emits the full summary payload whenever the session changes.

- event name: `summary`
- data: the same JSON shape returned by `GET /v1/sessions/{sessionId}/summary`
- idle periods: heartbeat comments are emitted to keep the connection alive

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
