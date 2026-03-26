#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import queue
import shlex
import subprocess
import sys
import threading
import time
import urllib.request
from dataclasses import dataclass


@dataclass
class ScenarioResult:
    feature_id: str
    state: str


def http_json(base_url: str, method: str, path: str, payload: dict[str, str] | None = None) -> dict:
    data = None if payload is None else json.dumps(payload).encode()
    request = urllib.request.Request(
        base_url.rstrip("/") + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def http_text(base_url: str, method: str, path: str) -> tuple[str, str]:
    request = urllib.request.Request(base_url.rstrip("/") + path, method=method)
    with urllib.request.urlopen(request) as response:
        return response.read().decode(), response.headers.get("Content-Type", "")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run_remote_helper(ssh_host: str, helper_path: str, args: list[str]) -> None:
    remote_command = " ".join([shlex.quote(helper_path), *[shlex.quote(arg) for arg in args]])
    result = subprocess.run(
        ["ssh", "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", ssh_host, f"sh -lc {shlex.quote(remote_command)}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return
    if result.stdout:
        sys.stderr.write(result.stdout)
    if result.stderr:
        sys.stderr.write(result.stderr)
    raise subprocess.CalledProcessError(result.returncode, result.args)


def create_session_response(base_url: str, environment_name: str) -> dict:
    return http_json(
        base_url,
        "POST",
        "/v1/sessions",
        {"agent_name": "smoke-feature-server", "environment_name": environment_name},
    )


def create_session(base_url: str, environment_name: str) -> str:
    return create_session_response(base_url, environment_name)["session_id"]


def start_feature(base_url: str, session_id: str, feature_id: str) -> dict:
    return http_json(base_url, "POST", f"/v1/sessions/{session_id}/features/{feature_id}/start", {})


def complete_feature(
    base_url: str,
    session_id: str,
    feature_id: str,
    evidence_kind: str,
    evidence_text: str,
    report_text: str,
) -> dict:
    return http_json(
        base_url,
        "POST",
        f"/v1/sessions/{session_id}/features/{feature_id}/complete",
        {
            "evidence_kind": evidence_kind,
            "evidence_text": evidence_text,
            "report_text": report_text,
        },
    )


def get_feature(base_url: str, session_id: str, feature_id: str) -> dict:
    return http_json(base_url, "GET", f"/v1/sessions/{session_id}/features/{feature_id}")


def get_summary(base_url: str, session_id: str) -> dict:
    return http_json(base_url, "GET", f"/v1/sessions/{session_id}/summary")


def wait_for_feature_state(
    base_url: str,
    session_id: str,
    feature_id: str,
    expected_states: set[str],
    timeout_seconds: float = 15.0,
) -> dict:
    deadline = time.monotonic() + timeout_seconds
    while True:
        feature = get_feature(base_url, session_id, feature_id)
        if feature["state"] in expected_states:
            return feature
        if time.monotonic() >= deadline:
            raise AssertionError(f"timed out waiting for {feature_id} to enter one of {sorted(expected_states)!r}")
        time.sleep(0.2)


def stream_summary_events(base_url: str, session_id: str, output: queue.Queue, stop_after: int) -> None:
    request = urllib.request.Request(
        base_url.rstrip("/") + f"/v1/sessions/{session_id}/summary/stream",
        headers={"Accept": "text/event-stream"},
        method="GET",
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            content_type = response.headers.get("Content-Type", "")
            require(content_type.startswith("text/event-stream"), f"unexpected stream content type {content_type!r}")
            event_name = ""
            data_lines: list[str] = []
            seen = 0
            while seen < stop_after:
                raw_line = response.readline()
                require(raw_line != b"", "summary stream closed before enough events were observed")
                line = raw_line.decode().rstrip("\r\n")
                if not line:
                    if not data_lines:
                        event_name = ""
                        continue
                    output.put({"event": event_name or "message", "data": json.loads("\n".join(data_lines))})
                    event_name = ""
                    data_lines = []
                    seen += 1
                    continue
                if line.startswith(":"):
                    continue
                if line.startswith("event:"):
                    event_name = line.partition(":")[2].strip()
                    continue
                if line.startswith("data:"):
                    data_lines.append(line.partition(":")[2].lstrip())
    except Exception as error:  # pragma: no cover - smoke utility surfaces the exact failure.
        output.put({"error": str(error)})


def next_stream_event(events: queue.Queue, timeout_seconds: float = 15.0) -> dict:
    item = events.get(timeout=timeout_seconds)
    if "error" in item:
        raise AssertionError(item["error"])
    return item


def feature_state_from_summary(summary: dict, feature_id: str) -> str:
    for feature in summary["features"]:
        if feature["id"] == feature_id:
            return feature["state"]
    raise AssertionError(f"feature {feature_id!r} missing from summary payload")


def run_basic(base_url: str, ssh_host: str, helper_path: str) -> ScenarioResult:
    session = create_session_response(base_url, "smoke-basic")
    session_id = session["session_id"]
    require(session["dashboard_path"] == f"/sessions/{session_id}/dashboard", "session response missing dashboard_path")

    summary = get_summary(base_url, session_id)
    require("pending" in summary and "active" in summary and "pending_or_active" in summary, "summary payload missing pending/active counts")

    dashboard_html, dashboard_content_type = http_text(base_url, "GET", session["dashboard_path"])
    require(dashboard_content_type.startswith("text/html"), f"unexpected dashboard content type {dashboard_content_type!r}")
    require(session_id in dashboard_html, "dashboard page does not include the session id")
    require(f"/v1/sessions/{session_id}/summary/stream" in dashboard_html, "dashboard page does not include the summary stream path")

    events: queue.Queue = queue.Queue()
    stream_thread = threading.Thread(target=stream_summary_events, args=(base_url, session_id, events, 3), daemon=True)
    stream_thread.start()
    initial_event = next_stream_event(events)
    require(initial_event["event"] == "summary", f"unexpected SSE event {initial_event['event']!r}")
    require(feature_state_from_summary(initial_event["data"], "bind_listen_connect") == "pending", "new session should start in pending state")

    started = start_feature(base_url, session_id, "bind_listen_connect")
    address = started["contract"]["connect_addresses"][0]
    run_remote_helper(
        ssh_host,
        helper_path,
        ["client", "--connect-addrs", address, "--messages", "bind-listen-connect:0:1"],
    )
    feature = wait_for_feature_state(base_url, session_id, "bind_listen_connect", {"passed"})
    stream_events = [initial_event, next_stream_event(events), next_stream_event(events)]
    stream_thread.join(timeout=1)
    states = [feature_state_from_summary(event["data"], "bind_listen_connect") for event in stream_events]
    require("active" in states, f"summary stream never reported active state: {states!r}")
    require("passed" in states, f"summary stream never reported passed state: {states!r}")
    final_summary = stream_events[-1]["data"]
    require(final_summary["active"] == 0, "final summary should not report any active scenarios")
    require(final_summary["passed"] >= 1, "final summary should report at least one passed scenario")
    return ScenarioResult("bind_listen_connect", feature["state"])


def run_hybrid(base_url: str, ssh_host: str, helper_path: str) -> ScenarioResult:
    session_id = create_session(base_url, "smoke-hybrid")
    started = start_feature(base_url, session_id, "nodelay")
    address = started["contract"]["connect_addresses"][0]
    run_remote_helper(
        ssh_host,
        helper_path,
        ["client", "--set-nodelay", "--connect-addrs", address, "--messages", "nodelay-check:3:31"],
    )
    feature = complete_feature(
        base_url,
        session_id,
        "nodelay",
        "runtime",
        "setsockopt(SCTP_NODELAY) succeeded",
        "client applied SCTP_NODELAY before connect",
    )
    return ScenarioResult("nodelay", feature["state"])


def run_agent_reported(base_url: str, ssh_host: str, helper_path: str) -> ScenarioResult:
    session_id = create_session(base_url, "smoke-agent-reported")
    started = start_feature(base_url, session_id, "negative_connect_error")
    target = started["contract"]["negative_connect_target"]
    run_remote_helper(
        ssh_host,
        helper_path,
        ["client", "--connect-addrs", target, "--expect-failure", "connect_or_send"],
    )
    feature = complete_feature(
        base_url,
        session_id,
        "negative_connect_error",
        "runtime",
        "connect failed against the invalid target as expected",
        "client surfaced the expected invalid-target failure",
    )
    return ScenarioResult("negative_connect_error", feature["state"])


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Smoke-test the FreeBSD SCTP feature server.")
    parser.add_argument("--base-url", required=True, help="HTTP base URL of the running feature server")
    parser.add_argument("--ssh-host", required=True, help="SSH host where the FreeBSD helper runs")
    parser.add_argument("--helper-path", required=True, help="Path to sctp-freebsd-helper on the FreeBSD host")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    results = [
        run_basic(args.base_url, args.ssh_host, args.helper_path),
        run_hybrid(args.base_url, args.ssh_host, args.helper_path),
        run_agent_reported(args.base_url, args.ssh_host, args.helper_path),
    ]
    summary = {result.feature_id: result.state for result in results}
    print(json.dumps(summary, sort_keys=True))
    return 0 if all(result.state == "passed" for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
