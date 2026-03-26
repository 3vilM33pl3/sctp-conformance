from __future__ import annotations

from dataclasses import dataclass, field
import json
import queue
import shutil
import shlex
import subprocess
import tarfile
import tempfile
import threading
import time
from pathlib import Path


def shell_join(parts: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in parts)


@dataclass(slots=True)
class CompletedCommand:
    returncode: int
    stdout: str
    stderr: str


@dataclass(slots=True)
class ManagedProcess:
    process: subprocess.Popen[str]
    name: str
    stdout_lines: list[str] = field(default_factory=list)
    stderr_lines: list[str] = field(default_factory=list)
    event_queue: queue.Queue[dict] = field(default_factory=queue.Queue)
    _stdout_thread: threading.Thread | None = None
    _stderr_thread: threading.Thread | None = None

    def start(self) -> "ManagedProcess":
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stdout_thread.start()
        self._stderr_thread.start()
        return self

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for raw_line in self.process.stdout:
            line = raw_line.rstrip("\n")
            self.stdout_lines.append(line)
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(event, dict):
                self.event_queue.put(event)

    def _read_stderr(self) -> None:
        assert self.process.stderr is not None
        for raw_line in self.process.stderr:
            self.stderr_lines.append(raw_line.rstrip("\n"))

    def wait_for_event(self, event_name: str, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"{self.name}: timed out waiting for {event_name}")
            event = self.event_queue.get(timeout=remaining)
            if event.get("event") == event_name:
                return event

    def wait(self, timeout: float | None = None) -> CompletedCommand:
        returncode = self.process.wait(timeout=timeout)
        if self._stdout_thread is not None:
            self._stdout_thread.join(timeout=1)
        if self._stderr_thread is not None:
            self._stderr_thread.join(timeout=1)
        return CompletedCommand(
            returncode=returncode,
            stdout="\n".join(self.stdout_lines),
            stderr="\n".join(self.stderr_lines),
        )

    def terminate(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)


class LocalExecutor:
    def run(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> CompletedCommand:
        completed = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return CompletedCommand(
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
        )

    def spawn(
        self,
        command: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        name: str,
    ) -> ManagedProcess:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return ManagedProcess(process=process, name=name).start()


class RemoteExecutor:
    def __init__(self, target: str):
        self.target = target

    def _ssh_command(self, script: str) -> list[str]:
        return ["ssh", "-o", "BatchMode=yes", self.target, f"sh -lc {shlex.quote(script)}"]

    def run(
        self,
        command: list[str],
        *,
        cwd: str | None = None,
        env: dict[str, str] | None = None,
        timeout: float | None = None,
    ) -> CompletedCommand:
        script_parts: list[str] = []
        if cwd:
            script_parts.append(f"cd {shlex.quote(cwd)}")
        if env:
            script_parts.extend(f"export {key}={shlex.quote(value)}" for key, value in env.items())
        script_parts.append(shell_join(command))
        completed = subprocess.run(
            self._ssh_command("; ".join(script_parts)),
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        return CompletedCommand(
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
        )

    def spawn(
        self,
        command: list[str],
        *,
        cwd: str | None = None,
        env: dict[str, str] | None = None,
        name: str,
    ) -> ManagedProcess:
        script_parts: list[str] = []
        if cwd:
            script_parts.append(f"cd {shlex.quote(cwd)}")
        if env:
            script_parts.extend(f"export {key}={shlex.quote(value)}" for key, value in env.items())
        script_parts.append(shell_join(command))
        process = subprocess.Popen(
            self._ssh_command("; ".join(script_parts)),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        return ManagedProcess(process=process, name=name).start()

    def stage_directory(self, local_dir: Path, remote_dir: str) -> None:
        mkdir = self.run(["mkdir", "-p", remote_dir])
        if mkdir.returncode != 0:
            raise RuntimeError(f"failed to create remote directory {remote_dir}: {mkdir.stderr}")
        tar_send = subprocess.Popen(
            ["tar", "-C", str(local_dir), "-cf", "-", "."],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        tar_recv = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", self.target, "tar", "-xf", "-", "-C", remote_dir],
            stdin=tar_send.stdout,
            text=False,
            capture_output=True,
            check=False,
        )
        assert tar_send.stdout is not None
        tar_send.stdout.close()
        send_err = tar_send.communicate()[1]
        if tar_send.returncode != 0:
            raise RuntimeError(send_err.decode("utf-8", errors="replace"))
        if tar_recv.returncode != 0:
            raise RuntimeError(tar_recv.stderr.decode("utf-8", errors="replace"))

    def copy_from_remote(self, remote_path: str, local_path: Path) -> None:
        local_path.parent.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(
            ["scp", "-q", f"{self.target}:{remote_path}", str(local_path)],
            text=True,
            capture_output=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(completed.stderr)
