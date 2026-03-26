from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
RUNNER_SRC = ROOT / "runner" / "src"

if str(RUNNER_SRC) not in sys.path:
    sys.path.insert(0, str(RUNNER_SRC))

from sctp_conformance.cli import main  # noqa: E402


if __name__ == "__main__":
    raise SystemExit(main())

