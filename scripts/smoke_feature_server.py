#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shlex
import subprocess
import sys
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


def create_session(base_url: str, environment_name: str) -> str:
    response = http_json(
        base_url,
        "POST",
        "/v1/sessions",
        {"agent_name": "smoke-feature-server", "environment_name": environment_name},
    )
    return response["session_id"]


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


def run_basic(base_url: str, ssh_host: str, helper_path: str) -> ScenarioResult:
    session_id = create_session(base_url, "smoke-basic")
    started = start_feature(base_url, session_id, "bind_listen_connect")
    address = started["contract"]["connect_addresses"][0]
    run_remote_helper(
        ssh_host,
        helper_path,
        ["client", "--connect-addrs", address, "--messages", "bind-listen-connect:0:1"],
    )
    feature = get_feature(base_url, session_id, "bind_listen_connect")
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
