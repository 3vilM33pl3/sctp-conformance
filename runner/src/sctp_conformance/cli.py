from __future__ import annotations

import argparse
from datetime import UTC, datetime
from pathlib import Path

from .config import load_profile, load_scenarios
from .report import write_coverage, write_junit, write_results, write_summary
from .runner import create_runner, evaluate_scenario, missing_required_features


def suite_root() -> Path:
    return Path(__file__).resolve().parents[3]


def profiles_dir(root: Path) -> Path:
    return root / "profiles"


def scenarios_file(root: Path) -> Path:
    return root / "scenarios" / "core.json"


def discover_profiles(root: Path) -> dict[str, Path]:
    return {path.stem: path for path in sorted(profiles_dir(root).glob("*.json"))}


def list_profiles() -> int:
    root = suite_root()
    for name in discover_profiles(root):
        print(name)
    return 0


def list_scenarios() -> int:
    root = suite_root()
    scenario_set = load_scenarios(scenarios_file(root))
    for scenario in scenario_set.scenarios:
        print(f"{scenario.id}: {scenario.title}")
    return 0


def run_suite(profile_name: str, scenario_names: list[str] | None) -> int:
    root = suite_root()
    profile_map = discover_profiles(root)
    if profile_name not in profile_map:
        raise SystemExit(f"unknown profile {profile_name!r}")
    profile = load_profile(profile_map[profile_name])
    scenario_set = load_scenarios(scenarios_file(root))
    scenarios = scenario_set.scenarios
    if scenario_names:
        wanted = set(scenario_names)
        scenarios = [scenario for scenario in scenarios if scenario.id in wanted]
    timestamp = datetime.now(tz=UTC).strftime("%Y%m%d-%H%M%S")
    run_dir = root / "artifacts" / "runs" / timestamp
    runner = create_runner(root, profile, run_dir)
    runner.bootstrap()
    runner.build()
    results = []
    for scenario in scenarios:
        artifacts = runner.execute(scenario)
        results.append(evaluate_scenario(profile, scenario, artifacts))
    missing: list[str] = []
    if scenario_names is None:
        missing = missing_required_features(results, scenario_set.required_features)
    if missing:
        from .report import ScenarioResult

        results.append(
            ScenarioResult(
                profile=profile.id,
                scenario_id="coverage",
                title="Coverage check",
                status="fail",
                message=f"missing required features: {', '.join(missing)}",
                required_features=[],
                stdout="",
                stderr="",
                events=[],
                artifacts={},
            )
        )
    run_dir.mkdir(parents=True, exist_ok=True)
    write_results(run_dir, results, scenario_set.required_features)
    write_summary(run_dir, results)
    write_coverage(run_dir, results, scenario_set.required_features)
    write_junit(run_dir, results)
    for result in results:
        print(f"{result.scenario_id}: {result.status} - {result.message}")
    return 1 if any(result.status == "fail" for result in results) else 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="sctp-conformance")
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("list-profiles")
    subparsers.add_parser("list-scenarios")

    run_parser = subparsers.add_parser("run")
    run_parser.add_argument("--profile", required=True)
    run_parser.add_argument("--scenario", action="append", dest="scenarios")

    args = parser.parse_args()
    if args.command == "list-profiles":
        return list_profiles()
    if args.command == "list-scenarios":
        return list_scenarios()
    if args.command == "run":
        return run_suite(args.profile, args.scenarios)
    return 1
