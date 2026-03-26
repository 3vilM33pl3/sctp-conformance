from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
from typing import Any


@dataclass(slots=True)
class InitOptions:
    num_ostreams: int = 0
    max_instreams: int = 0
    max_attempts: int = 0
    max_init_timeout: int = 0


@dataclass(slots=True)
class MessageSpec:
    payload: str
    stream: int
    ppid: int


@dataclass(slots=True)
class EndpointSpec:
    bind_addrs: list[str] = field(default_factory=list)
    connect_addrs: list[str] = field(default_factory=list)
    connect_from_server_ready: bool = False
    subscribe: list[str] = field(default_factory=list)
    init_options: InitOptions = field(default_factory=InitOptions)
    set_nodelay: bool = False
    emit_local_addrs: bool = False
    emit_peer_addrs: bool = False
    read_messages: int = 0
    expect_failure: str | None = None
    messages: list[MessageSpec] = field(default_factory=list)


@dataclass(slots=True)
class Scenario:
    id: str
    title: str
    required_features: list[str]
    server: EndpointSpec | None = None
    client: EndpointSpec | None = None
    assertions: dict[str, Any] = field(default_factory=dict)
    capture_pcap: bool = False


@dataclass(slots=True)
class ScenarioSet:
    required_features: list[str]
    scenarios: list[Scenario]


@dataclass(slots=True)
class Profile:
    id: str
    kind: str
    raw: dict[str, Any]

    def path_value(self, root: Path, key: str) -> Path:
        return (root / self.raw[key]).resolve()

    def string_value(self, key: str) -> str:
        return str(self.raw[key])

    def list_value(self, key: str) -> list[str]:
        return list(self.raw.get(key, []))


def _load_endpoint(data: dict[str, Any] | None) -> EndpointSpec | None:
    if not data:
        return None
    init_raw = data.get("init_options", {})
    return EndpointSpec(
        bind_addrs=list(data.get("bind_addrs", [])),
        connect_addrs=list(data.get("connect_addrs", [])),
        connect_from_server_ready=bool(data.get("connect_from_server_ready", False)),
        subscribe=list(data.get("subscribe", [])),
        init_options=InitOptions(
            num_ostreams=int(init_raw.get("num_ostreams", 0)),
            max_instreams=int(init_raw.get("max_instreams", 0)),
            max_attempts=int(init_raw.get("max_attempts", 0)),
            max_init_timeout=int(init_raw.get("max_init_timeout", 0)),
        ),
        set_nodelay=bool(data.get("set_nodelay", False)),
        emit_local_addrs=bool(data.get("emit_local_addrs", False)),
        emit_peer_addrs=bool(data.get("emit_peer_addrs", False)),
        read_messages=int(data.get("read_messages", 0)),
        expect_failure=data.get("expect_failure"),
        messages=[
            MessageSpec(
                payload=str(message["payload"]),
                stream=int(message["stream"]),
                ppid=int(message["ppid"]),
            )
            for message in data.get("messages", [])
        ],
    )


def load_scenarios(path: Path) -> ScenarioSet:
    raw = json.loads(path.read_text())
    scenarios = [
        Scenario(
            id=str(item["id"]),
            title=str(item["title"]),
            required_features=list(item.get("required_features", [])),
            server=_load_endpoint(item.get("server")),
            client=_load_endpoint(item.get("client")),
            assertions=dict(item.get("assertions", {})),
            capture_pcap=bool(item.get("capture_pcap", False)),
        )
        for item in raw["scenarios"]
    ]
    return ScenarioSet(required_features=list(raw["required_features"]), scenarios=scenarios)


def load_profile(path: Path) -> Profile:
    raw = json.loads(path.read_text())
    return Profile(id=str(raw["id"]), kind=str(raw["kind"]), raw=raw)

