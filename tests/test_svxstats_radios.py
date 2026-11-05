"""Run SvxStats radio parsing, history, persistence, and FRN formatting tests.

The host harness uses production sources, omitting only the asynchronous
process/timer entry points so no installed SvxLink or hardware is needed.
Usage: python3 tests/test_svxstats_radios.py [--snapshot OUTPUT]
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    stats_dir = root / "src/svxlink/svxlink"
    source = (stats_dir / "SvxStats.cpp").read_text()
    for include in ("AsyncExec.h", "AsyncTimer.h", "config.h", "version/SVXLINK.h"):
        source = source.replace(f"#include <{include}>", "")
    start = source.index("void SvxStats::start(uint32_t interval_s)")
    end = source.index("uint64_t SvxStats::uptimeSeconds()", start)
    source = source[:start] + source[end:]
    start = source.index("void SvxStats::collectAiorsDiagnostics()")
    end = source.index("void SvxStats::recordAiorsFailure", start)
    source = source[:start] + source[end:]
    frn = (root / "src/svxlink/modules/frn/ModuleFrn.cpp").read_text()
    allowlist = frn[frn.index("static inline const std::vector<std::string>& supportedRunCmds()"):
                    frn.index("static inline std::string runCmdHelpLine()")]
    prefix = """#include <bits/stdc++.h>
#define private public
#include "SvxStats.h"
#undef private
#define SVXLINK_APP_VERSION "test"
#define PROJECT_VERSION "test"
"""
    tests = (root / "tests/svxstats_radios.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="svxstats-radios-") as temp:
        unit = Path(temp) / "radios.cpp"
        binary = Path(temp) / "radios"
        unit.write_text(prefix + source + "\n" + allowlist + "\n" + tests)
        extra = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-fno-sanitize-recover=all"] if args.sanitizers else []
        subprocess.run(["g++", *extra, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-pthread", "-I", str(stats_dir), str(unit), "-o", str(binary)], check=True)
        command = [str(binary), str(root / "tests/aiors_diagnostics_v5.txt"), str(Path(temp) / "totals")]
        if args.snapshot:
            command.append(str(args.snapshot.resolve()))
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
