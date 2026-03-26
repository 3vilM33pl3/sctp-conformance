from __future__ import annotations

from dataclasses import dataclass, asdict
import json
from pathlib import Path
from typing import Any
from xml.etree import ElementTree as ET


@dataclass(slots=True)
class ScenarioResult:
    profile: str
    scenario_id: str
    title: str
    status: str
    message: str
    required_features: list[str]
    stdout: str
    stderr: str
    events: list[dict[str, Any]]
    artifacts: dict[str, str]


def write_results(run_dir: Path, results: list[ScenarioResult], required_features: list[str]) -> None:
    serializable = {
        "required_features": required_features,
        "results": [asdict(result) for result in results],
    }
    (run_dir / "results.json").write_text(json.dumps(serializable, indent=2, sort_keys=True))


def write_summary(run_dir: Path, results: list[ScenarioResult]) -> None:
    lines = ["# SCTP Conformance Summary", ""]
    grouped: dict[str, list[ScenarioResult]] = {}
    for result in results:
        grouped.setdefault(result.profile, []).append(result)
    for profile, profile_results in sorted(grouped.items()):
        lines.append(f"## {profile}")
        lines.append("")
        for result in profile_results:
            lines.append(f"- `{result.scenario_id}`: **{result.status.upper()}** - {result.message}")
        lines.append("")
    (run_dir / "summary.md").write_text("\n".join(lines))


def write_coverage(run_dir: Path, results: list[ScenarioResult], required_features: list[str]) -> None:
    lines = ["# SCTP Feature Coverage", ""]
    grouped: dict[str, dict[str, list[str]]] = {}
    for result in results:
        if result.scenario_id == "coverage":
            continue
        coverage = grouped.setdefault(result.profile, {feature: [] for feature in required_features})
        for feature in result.required_features:
            coverage.setdefault(feature, []).append(f"{result.scenario_id} ({result.status})")
    for profile, coverage in sorted(grouped.items()):
        lines.append(f"## {profile}")
        lines.append("")
        for feature in required_features:
            scenarios = coverage.get(feature, [])
            if scenarios:
                lines.append(f"- `{feature}`: {', '.join(scenarios)}")
            else:
                lines.append(f"- `{feature}`: MISSING")
        lines.append("")
    (run_dir / "coverage.md").write_text("\n".join(lines))


def write_junit(run_dir: Path, results: list[ScenarioResult]) -> None:
    suites = ET.Element("testsuites")
    grouped: dict[str, list[ScenarioResult]] = {}
    for result in results:
        grouped.setdefault(result.profile, []).append(result)
    for profile, profile_results in sorted(grouped.items()):
        suite = ET.SubElement(
            suites,
            "testsuite",
            name=profile,
            tests=str(len(profile_results)),
            failures=str(sum(1 for result in profile_results if result.status == "fail")),
            skipped=str(sum(1 for result in profile_results if result.status == "unsupported")),
        )
        for result in profile_results:
            case = ET.SubElement(suite, "testcase", name=result.scenario_id, classname=profile)
            if result.status == "fail":
                failure = ET.SubElement(case, "failure", message=result.message)
                failure.text = result.stderr or result.stdout
            elif result.status == "unsupported":
                skipped = ET.SubElement(case, "skipped", message=result.message)
                skipped.text = result.stderr or result.stdout
    tree = ET.ElementTree(suites)
    tree.write(run_dir / "junit.xml", encoding="utf-8", xml_declaration=True)
