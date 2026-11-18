import os
import shlex
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "components" / "climate_adaptivepid"


def main():
    compiler = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="adaptivepid-tests-") as build_dir:
        for test in sorted((ROOT / "tests").glob("test_*.cpp")):
            print(f"Running {test.stem}", flush=True)
            binary = Path(build_dir) / test.stem
            sources = [test]
            if test.stem == "test_adaptive_pid":
                sources.append(COMPONENT / "adaptive_pid.cpp")
            flags = ["-std=c++17", "-Wall", "-Wextra", "-I", COMPONENT]
            subprocess.run([*compiler, *flags, "-o", binary, *sources], check=True)
            subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()
