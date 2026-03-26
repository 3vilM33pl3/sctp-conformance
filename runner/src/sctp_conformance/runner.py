from __future__ import annotations

from dataclasses import dataclass, replace
import json
import os
import socket
from pathlib import Path
from typing import Any

from .config import EndpointSpec, Profile, Scenario
from .process import CompletedCommand, LocalExecutor, RemoteExecutor
from .report import ScenarioResult


NOTIFICATION_EVENT = "notify"
UNSUPPORTED_MARKERS = (
    "not supported",
    "unsupported",
    "unavailable",
    "operation not supported",
    "protocol not supported",
)


def resolve_root_password_file(profile: Profile) -> tuple[Path | None, str | None]:
    env_name = profile.raw.get("root_password_file_env")
    if env_name is not None:
        env_value = os.environ.get(str(env_name))
        if env_value:
            return Path(env_value), str(env_name)
        return None, str(env_name)
    raw_value = profile.raw.get("root_password_file")
    if raw_value is None:
        return None, None
    return Path(str(raw_value)), None


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def encode_messages(messages: list[Any]) -> str:
    return ",".join(f"{message.payload}:{message.stream}:{message.ppid}" for message in messages)


def command_flags(endpoint: EndpointSpec, mode: str) -> list[str]:
    flags: list[str] = [mode]
    if endpoint.bind_addrs:
        flags.extend(["--bind-addrs", ",".join(endpoint.bind_addrs)])
    if endpoint.connect_addrs:
        flags.extend(["--connect-addrs", ",".join(endpoint.connect_addrs)])
    if endpoint.subscribe:
        flags.extend(["--subscribe", ",".join(endpoint.subscribe)])
    if endpoint.read_messages:
        flags.extend(["--read-messages", str(endpoint.read_messages)])
    if endpoint.set_nodelay:
        flags.append("--set-nodelay")
    if endpoint.emit_local_addrs:
        flags.append("--emit-local-addrs")
    if endpoint.emit_peer_addrs:
        flags.append("--emit-peer-addrs")
    if endpoint.expect_failure:
        flags.extend(["--expect-failure", endpoint.expect_failure])
    if endpoint.messages:
        flags.extend(["--messages", encode_messages(endpoint.messages)])
    init = endpoint.init_options
    if any([init.num_ostreams, init.max_instreams, init.max_attempts, init.max_init_timeout]):
        flags.extend(
            [
                "--init-ostreams",
                str(init.num_ostreams),
                "--init-instreams",
                str(init.max_instreams),
                "--init-attempts",
                str(init.max_attempts),
                "--init-timeout",
                str(init.max_init_timeout),
            ]
        )
    return flags


def extract_events(output: str) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    for line in output.splitlines():
        try:
            item = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(item, dict):
            events.append(item)
    return events


def extract_events_for_role(output: str, role: str) -> list[dict[str, Any]]:
    events = extract_events(output)
    for event in events:
        event["role"] = role
    return events


def first_event(events: list[dict[str, Any]], event_name: str) -> dict[str, Any] | None:
    for event in events:
        if event.get("event") == event_name:
            return event
    return None


def matching_events(events: list[dict[str, Any]], event_name: str) -> list[dict[str, Any]]:
    return [event for event in events if event.get("event") == event_name]


def apply_port(addrs: list[str], port: int) -> list[str]:
    rewritten: list[str] = []
    for item in addrs:
        host, _, _ = item.rpartition(":")
        rewritten.append(f"{host}:{port}")
    return rewritten


@dataclass(slots=True)
class ScenarioArtifacts:
    stdout: str
    stderr: str
    events: list[dict[str, Any]]
    artifact_paths: dict[str, str]


class ProfileRunner:
    def __init__(self, suite_root: Path, profile: Profile, run_dir: Path):
        self.suite_root = suite_root
        self.profile = profile
        self.run_dir = run_dir

    def build(self) -> None:
        raise NotImplementedError

    def bootstrap(self) -> None:
        return None

    def execute(self, scenario: Scenario) -> ScenarioArtifacts:
        raise NotImplementedError


class LocalGoRunner(ProfileRunner):
    def __init__(self, suite_root: Path, profile: Profile, run_dir: Path):
        super().__init__(suite_root, profile, run_dir)
        self.executor = LocalExecutor()
        self.go_repo = profile.path_value(suite_root, "go_repo")
        self.go_bin = profile.path_value(suite_root, "go_bin")
        self.helper_source = profile.path_value(suite_root, "helper_source")
        self.helper_binary = profile.path_value(suite_root, "helper_binary")

    def build(self) -> None:
        self.helper_binary.parent.mkdir(parents=True, exist_ok=True)
        env = {**os.environ, "GOROOT": str(self.go_repo)}
        completed = self.executor.run(
            [str(self.go_bin), "build", "-o", str(self.helper_binary), "./cmd/sctp-helper"],
            cwd=self.helper_source,
            env=env,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr or completed.stdout)

    def execute(self, scenario: Scenario) -> ScenarioArtifacts:
        scenario_dir = self.run_dir / self.profile.id / scenario.id
        scenario_dir.mkdir(parents=True, exist_ok=True)
        if scenario.server is None:
            return self._run_client_only(scenario, scenario_dir)
        return self._run_server_client(scenario, scenario_dir)

    def _run_server_client(self, scenario: Scenario, scenario_dir: Path) -> ScenarioArtifacts:
        assert scenario.server is not None
        assert scenario.client is not None
        server_cmd = [str(self.helper_binary), *command_flags(scenario.server, "server")]
        server = self.executor.spawn(server_cmd, name=f"{self.profile.id}:{scenario.id}:server")
        try:
            ready = server.wait_for_event("ready", timeout=10)
            connect_addrs = list(ready.get("local_addrs", []))
            client_spec = scenario.client
            if client_spec.connect_from_server_ready:
                client_spec = replace(client_spec, connect_addrs=connect_addrs)
            client_cmd = [str(self.helper_binary), *command_flags(client_spec, "client")]
            client = self.executor.run(client_cmd, timeout=10)
            server_result = server.wait(timeout=10)
        finally:
            server.terminate()
        stdout = "\n".join([server_result.stdout, client.stdout]).strip()
        stderr = "\n".join([server_result.stderr, client.stderr]).strip()
        events = extract_events_for_role(server_result.stdout, "server") + extract_events_for_role(client.stdout, "client")
        artifact_paths = self._write_artifacts(scenario_dir, server_result, client)
        return ScenarioArtifacts(stdout=stdout, stderr=stderr, events=events, artifact_paths=artifact_paths)

    def _run_client_only(self, scenario: Scenario, scenario_dir: Path) -> ScenarioArtifacts:
        assert scenario.client is not None
        client_spec = scenario.client
        if not client_spec.connect_addrs:
            client_spec = replace(client_spec, connect_addrs=apply_port(["127.0.0.1:0"], free_port()))
        client_cmd = [str(self.helper_binary), *command_flags(client_spec, "client")]
        client = self.executor.run(client_cmd, timeout=10)
        events = extract_events_for_role(client.stdout, "client")
        artifact_paths = self._write_client_only_artifacts(scenario_dir, client)
        return ScenarioArtifacts(stdout=client.stdout, stderr=client.stderr, events=events, artifact_paths=artifact_paths)

    def _write_artifacts(self, scenario_dir: Path, server: CompletedCommand, client: CompletedCommand) -> dict[str, str]:
        (scenario_dir / "server.stdout").write_text(server.stdout)
        (scenario_dir / "server.stderr").write_text(server.stderr)
        (scenario_dir / "client.stdout").write_text(client.stdout)
        (scenario_dir / "client.stderr").write_text(client.stderr)
        return {
            "server_stdout": str((scenario_dir / "server.stdout").relative_to(self.run_dir)),
            "server_stderr": str((scenario_dir / "server.stderr").relative_to(self.run_dir)),
            "client_stdout": str((scenario_dir / "client.stdout").relative_to(self.run_dir)),
            "client_stderr": str((scenario_dir / "client.stderr").relative_to(self.run_dir)),
        }

    def _write_client_only_artifacts(self, scenario_dir: Path, client: CompletedCommand) -> dict[str, str]:
        (scenario_dir / "client.stdout").write_text(client.stdout)
        (scenario_dir / "client.stderr").write_text(client.stderr)
        return {
            "client_stdout": str((scenario_dir / "client.stdout").relative_to(self.run_dir)),
            "client_stderr": str((scenario_dir / "client.stderr").relative_to(self.run_dir)),
        }


class FreeBSDRunner(ProfileRunner):
    def __init__(self, suite_root: Path, profile: Profile, run_dir: Path):
        super().__init__(suite_root, profile, run_dir)
        password_file, password_file_env = resolve_root_password_file(profile)
        self.executor = RemoteExecutor(profile.string_value("ssh_target"), password_file=password_file)
        self.remote_workdir = profile.string_value("remote_workdir")
        self.helper_source = profile.path_value(suite_root, "helper_source")
        self.helper_binary = profile.string_value("helper_binary")
        self.loopback_aliases = profile.list_value("loopback_aliases")
        self.tcpdump_interface = profile.string_value("tcpdump_interface")
        self.password_file_env = password_file_env

    def bootstrap(self) -> None:
        self.executor.run(["mkdir", "-p", self.remote_workdir])
        module_state = self.executor.run(["kldstat"], timeout=20)
        module_loaded = " sctp.ko" in module_state.stdout
        if not module_loaded:
            if self.executor.password_file is None:
                detail = f" via ${self.password_file_env}" if self.password_file_env else ""
                raise RuntimeError(
                    f"FreeBSD SCTP module is not loaded; load sctp.ko manually or provide a root password file{detail}"
                )
            load = self.executor.run_root("kldload /boot/kernel/sctp.ko", timeout=20)
            if load.returncode != 0:
                raise RuntimeError(load.stderr or load.stdout)
        loopback_state = self.executor.run(["ifconfig", "lo0"], timeout=20)
        for alias in self.loopback_aliases:
            alias_ip = alias.split("/", 1)[0]
            if alias_ip in loopback_state.stdout:
                continue
            if self.executor.password_file is None:
                detail = f" via ${self.password_file_env}" if self.password_file_env else ""
                raise RuntimeError(
                    f"loopback alias {alias} is missing; add it manually or provide a root password file{detail}"
                )
            completed = self.executor.run_root(f"ifconfig lo0 alias {alias}", timeout=20)
            if completed.returncode != 0:
                raise RuntimeError(completed.stderr or completed.stdout)

    def build(self) -> None:
        remote_src = f"{self.remote_workdir}/freebsd_c"
        self.executor.run(["rm", "-rf", remote_src])
        self.executor.run(["mkdir", "-p", remote_src])
        self.executor.stage_directory(self.helper_source, remote_src)
        completed = self.executor.run(["make", "clean", "all"], cwd=remote_src, timeout=30)
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr or completed.stdout)

    def execute(self, scenario: Scenario) -> ScenarioArtifacts:
        scenario_dir = self.run_dir / self.profile.id / scenario.id
        scenario_dir.mkdir(parents=True, exist_ok=True)
        if scenario.server is None:
            return self._run_client_only(scenario, scenario_dir)
        return self._run_server_client(scenario, scenario_dir)

    def _remote_binary(self) -> str:
        return f"{self.remote_workdir}/freebsd_c/{self.helper_binary}"

    def _run_server_client(self, scenario: Scenario, scenario_dir: Path) -> ScenarioArtifacts:
        assert scenario.server is not None
        assert scenario.client is not None
        pcap_remote = None
        tcpdump_pid = None
        server_cmd = [self._remote_binary(), *command_flags(scenario.server, "server")]
        server = self.executor.spawn(server_cmd, name=f"{self.profile.id}:{scenario.id}:server")
        try:
            ready = server.wait_for_event("ready", timeout=10)
            connect_addrs = list(ready.get("local_addrs", []))
            if scenario.capture_pcap and connect_addrs and self.executor.password_file is not None:
                port = int(connect_addrs[0].rsplit(":", 1)[1])
                pcap_remote = f"{self.remote_workdir}/{scenario.id}.pcap"
                started = self.executor.run_root(
                    f"sh -c 'tcpdump -U -i {self.tcpdump_interface} -w {pcap_remote} port {port} >/tmp/{scenario.id}.tcpdump.log 2>&1 & echo $!'",
                    timeout=20,
                )
                if started.returncode == 0:
                    tcpdump_pid = started.stdout.strip().splitlines()[-1] if started.stdout.strip() else None
            client_spec = scenario.client
            if client_spec.connect_from_server_ready:
                client_spec = replace(client_spec, connect_addrs=connect_addrs)
            client_cmd = [self._remote_binary(), *command_flags(client_spec, "client")]
            client = self.executor.run(client_cmd, timeout=15)
            server_result = server.wait(timeout=15)
        finally:
            if tcpdump_pid:
                self.executor.run_root(f"kill {tcpdump_pid} || true", timeout=20)
            server.terminate()
        stdout = "\n".join([server_result.stdout, client.stdout]).strip()
        stderr = "\n".join([server_result.stderr, client.stderr]).strip()
        events = extract_events_for_role(server_result.stdout, "server") + extract_events_for_role(client.stdout, "client")
        artifact_paths = self._write_artifacts(scenario_dir, server_result, client, pcap_remote)
        return ScenarioArtifacts(stdout=stdout, stderr=stderr, events=events, artifact_paths=artifact_paths)

    def _run_client_only(self, scenario: Scenario, scenario_dir: Path) -> ScenarioArtifacts:
        assert scenario.client is not None
        client_cmd = [self._remote_binary(), *command_flags(scenario.client, "client")]
        client = self.executor.run(client_cmd, timeout=15)
        events = extract_events_for_role(client.stdout, "client")
        artifact_paths = self._write_client_only_artifacts(scenario_dir, client)
        return ScenarioArtifacts(stdout=client.stdout, stderr=client.stderr, events=events, artifact_paths=artifact_paths)

    def _write_artifacts(
        self,
        scenario_dir: Path,
        server: CompletedCommand,
        client: CompletedCommand,
        pcap_remote: str | None,
    ) -> dict[str, str]:
        (scenario_dir / "server.stdout").write_text(server.stdout)
        (scenario_dir / "server.stderr").write_text(server.stderr)
        (scenario_dir / "client.stdout").write_text(client.stdout)
        (scenario_dir / "client.stderr").write_text(client.stderr)
        artifacts = {
            "server_stdout": str((scenario_dir / "server.stdout").relative_to(self.run_dir)),
            "server_stderr": str((scenario_dir / "server.stderr").relative_to(self.run_dir)),
            "client_stdout": str((scenario_dir / "client.stdout").relative_to(self.run_dir)),
            "client_stderr": str((scenario_dir / "client.stderr").relative_to(self.run_dir)),
        }
        if pcap_remote:
            local_pcap = scenario_dir / "capture.pcap"
            try:
                self.executor.copy_from_remote(pcap_remote, local_pcap)
                artifacts["pcap"] = str(local_pcap.relative_to(self.run_dir))
            except RuntimeError:
                pass
        return artifacts

    def _write_client_only_artifacts(self, scenario_dir: Path, client: CompletedCommand) -> dict[str, str]:
        (scenario_dir / "client.stdout").write_text(client.stdout)
        (scenario_dir / "client.stderr").write_text(client.stderr)
        return {
            "client_stdout": str((scenario_dir / "client.stdout").relative_to(self.run_dir)),
            "client_stderr": str((scenario_dir / "client.stderr").relative_to(self.run_dir)),
        }


def evaluate_scenario(profile: Profile, scenario: Scenario, artifacts: ScenarioArtifacts) -> ScenarioResult:
    events = artifacts.events
    recv_events = matching_events(events, "recv")
    notify_events = matching_events(events, NOTIFICATION_EVENT)
    message = "passed"
    status = "pass"
    error_event = first_event(events, "error")

    if error_event is not None:
        message = str(error_event.get("message", "helper reported an error"))
        lowered = message.lower()
        status = "unsupported" if any(marker in lowered for marker in UNSUPPORTED_MARKERS) else "fail"
        return ScenarioResult(
            profile=profile.id,
            scenario_id=scenario.id,
            title=scenario.title,
            status=status,
            message=message,
            required_features=scenario.required_features,
            stdout=artifacts.stdout,
            stderr=artifacts.stderr,
            events=events,
            artifacts=artifacts.artifact_paths,
        )

    expected_failure = scenario.assertions.get("expected_failure")
    if expected_failure:
        expected_stages = (
            [str(stage) for stage in expected_failure]
            if isinstance(expected_failure, list)
            else [str(expected_failure)]
        )
        matched = first_event(events, "expected_failure")
        if matched is None or matched.get("stage") not in expected_stages:
            status = "fail"
            message = f"expected failure stages {expected_stages!r} were not observed"
        return ScenarioResult(
            profile=profile.id,
            scenario_id=scenario.id,
            title=scenario.title,
            status=status,
            message=message,
            required_features=scenario.required_features,
            stdout=artifacts.stdout,
            stderr=artifacts.stderr,
            events=events,
            artifacts=artifacts.artifact_paths,
        )

    if len(recv_events) != int(scenario.assertions.get("server_recv_count", len(recv_events))):
        status = "fail"
        message = f"expected {scenario.assertions.get('server_recv_count')} recv events, got {len(recv_events)}"
    expected_payloads = scenario.assertions.get("server_payloads")
    if status == "pass" and expected_payloads is not None:
        actual_payloads = [event.get("payload") for event in recv_events]
        if actual_payloads != expected_payloads:
            status = "fail"
            message = f"payload mismatch: expected {expected_payloads}, got {actual_payloads}"
    expected_streams = scenario.assertions.get("server_streams")
    if status == "pass" and expected_streams is not None:
        actual_streams = [event.get("stream") for event in recv_events]
        if actual_streams != expected_streams:
            status = "fail"
            message = f"stream mismatch: expected {expected_streams}, got {actual_streams}"
    expected_ppids = scenario.assertions.get("server_ppids")
    if status == "pass" and expected_ppids is not None:
        actual_ppids = [event.get("ppid") for event in recv_events]
        if actual_ppids != expected_ppids:
            status = "fail"
            message = f"PPID mismatch: expected {expected_ppids}, got {actual_ppids}"
    min_notify = scenario.assertions.get("min_notify_count")
    if status == "pass" and min_notify is not None and len(notify_events) < int(min_notify):
        status = "fail"
        message = f"expected at least {min_notify} notifications, got {len(notify_events)}"
    local_addr_count = scenario.assertions.get("server_local_addr_count")
    if status == "pass" and local_addr_count is not None:
        ready = next((event for event in events if event.get("event") == "ready" and event.get("role") == "server"), None)
        actual = len(list(ready.get("local_addrs", []))) if ready else 0
        if actual != int(local_addr_count):
            status = "fail"
            message = f"expected server local addr count {local_addr_count}, got {actual}"
    peer_addr_count = scenario.assertions.get("client_peer_addr_count")
    if status == "pass" and peer_addr_count is not None:
        peer_event = next(
            (event for event in events if event.get("event") == "peer_addrs" and event.get("role") == "client"),
            None,
        )
        actual = len(list(peer_event.get("addrs", []))) if peer_event else 0
        if actual != int(peer_addr_count):
            status = "fail"
            message = f"expected client peer addr count {peer_addr_count}, got {actual}"
    return ScenarioResult(
        profile=profile.id,
        scenario_id=scenario.id,
        title=scenario.title,
        status=status,
        message=message,
        required_features=scenario.required_features,
        stdout=artifacts.stdout,
        stderr=artifacts.stderr,
        events=events,
        artifacts=artifacts.artifact_paths,
    )


def create_runner(suite_root: Path, profile: Profile, run_dir: Path) -> ProfileRunner:
    if profile.kind == "local-go-net":
        return LocalGoRunner(suite_root, profile, run_dir)
    if profile.kind == "remote-freebsd-c":
        return FreeBSDRunner(suite_root, profile, run_dir)
    raise ValueError(f"unsupported profile kind {profile.kind}")


def missing_required_features(results: list[ScenarioResult], required_features: list[str]) -> list[str]:
    covered: set[str] = set()
    for result in results:
        if result.scenario_id == "coverage":
            continue
        covered.update(result.required_features)
    return [feature for feature in required_features if feature not in covered]
