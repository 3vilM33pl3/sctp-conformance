from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
import queue
import selectors
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
    def __init__(self, target: str, password_file: Path | None = None):
        self.target = target
        self.password_file = password_file

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

    def run_root(self, shell_command: str, *, timeout: float | None = None) -> CompletedCommand:
        if self.password_file is None:
            raise RuntimeError("root password file is required for root operations")
        password = self.password_file.read_text().strip().encode("utf-8") + b"\n"
        process = subprocess.Popen(
            [
                "ssh",
                "-tt",
                "-o",
                "BatchMode=yes",
                self.target,
                f"su -m root -c {shlex.quote(shell_command)}",
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
        )
        assert process.stdin is not None
        assert process.stdout is not None
        assert process.stderr is not None

        selector = selectors.DefaultSelector()
        selector.register(process.stdout, selectors.EVENT_READ, data="stdout")
        selector.register(process.stderr, selectors.EVENT_READ, data="stderr")
        stdout = bytearray()
        stderr = bytearray()
        prompt_window = bytearray()
        password_sent = False
        deadline = None if timeout is None else time.monotonic() + timeout

        while selector.get_map():
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                process.kill()
                process.wait(timeout=5)
                raise subprocess.TimeoutExpired(process.args, timeout)
            events = selector.select(remaining)
            if not events:
                continue
            for key, _ in events:
                chunk = os.read(key.fileobj.fileno(), 1024)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                if key.data == "stdout":
                    stdout.extend(chunk)
                else:
                    stderr.extend(chunk)
                prompt_window.extend(chunk)
                if len(prompt_window) > 256:
                    del prompt_window[:-256]
                if not password_sent and b"assword:" in prompt_window:
                    process.stdin.write(password)
                    process.stdin.flush()
                    password_sent = True

        returncode = process.wait(timeout=max(0.0, deadline - time.monotonic()) if deadline is not None else None)
        return CompletedCommand(
            returncode=returncode,
            stdout=stdout.decode("utf-8", errors="replace"),
            stderr=stderr.decode("utf-8", errors="replace"),
        )

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
