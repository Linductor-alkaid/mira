"""External process termination; marker polling belongs to the CTest driver only."""

import argparse
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time


STAGES = (
    "before_prepare", "after_prepare", "before_start", "after_start",
    "after_execute", "before_receipt", "after_receipt", "after_uncertain", "terminal",
)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--root", required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    # Each run owns a new directory; never recursively delete a supplied path.
    run = Path(tempfile.mkdtemp(prefix="external-kill-", dir=root))
    passed = False
    try:
        for stage in STAGES:
            case = run / stage
            writer = subprocess.Popen(
                [str(probe), "write-kill", str(case), stage],
                stdin=subprocess.PIPE,
            )
            try:
                ready = case / "kill-ready"
                deadline = time.monotonic() + 10
                while not ready.exists() or ready.read_bytes() != (stage + "\n").encode():
                    if writer.poll() is not None:
                        raise RuntimeError(f"{stage}: writer exited before handshake: {writer.returncode}")
                    if time.monotonic() >= deadline:
                        raise TimeoutError(f"{stage}: boundary handshake timed out")
                    time.sleep(0.005)
                if writer.poll() is not None:
                    raise RuntimeError(f"{stage}: writer did not hold the boundary")
                writer.kill()
                code = writer.wait(timeout=10)
                if os.name == "posix":
                    if code != -signal.SIGKILL:
                        raise RuntimeError(f"{stage}: expected SIGKILL, got {code}")
                elif code == 0:
                    raise RuntimeError(f"{stage}: expected external termination")
            finally:
                if writer.poll() is None:
                    writer.kill()
                    writer.wait(timeout=10)
                writer.stdin.close()
            subprocess.run([str(probe), "verify", str(case), stage], check=True, timeout=10)
            print(f"{stage}: external termination {code}, independent recovery passed", flush=True)
        passed = True
    finally:
        if passed:
            shutil.rmtree(run)
        else:
            print(f"failure evidence preserved: {run}", flush=True)


if __name__ == "__main__":
    main()
