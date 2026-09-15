#!/usr/bin/env python3
"""Turn the first compile errors of a log file into check annotations.

    python3 tools/ci/annotate.py build.log [--limit 20]

GitHub only serves job logs from a blob host that a sandboxed or proxied
runner cannot always reach, while annotations are part of the Checks API and
survive that.  Printing the important lines as `::error::` therefore keeps a
failing CI run debuggable from the command line:

    gh api repos/<owner>/<repo>/check-runs/<job-id>/annotations

Exits 0 always — this tool reports, it does not decide.
"""

from __future__ import annotations

import argparse
import re
import sys

ERROR_LINE = re.compile(r"(error:|Error:|FAILED|ninja: build stopped|CMake Error"
                        r"|undefined reference|No such file or directory)")
NOISE = re.compile(r"^\s*(make\[\d+\]|g?make\s|ninja: entering|In file included from)")


def escape(text: str) -> str:
    return text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A").replace(":", "%3A", 1)


def main() -> int:
    parser = argparse.ArgumentParser(prog="annotate", description=__doc__.splitlines()[0])
    parser.add_argument("log", nargs="?", help="file to read; default stdin")
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--context", type=int, default=0, help="also print N lines before each error")
    args = parser.parse_args()

    try:
        text = open(args.log, encoding="utf-8", errors="replace").read() if args.log else sys.stdin.read()
    except OSError as error:
        print(f"::error title=CI annotator::{escape(str(error))}")
        return 0

    lines = text.splitlines()
    shown = 0
    for index, line in enumerate(lines):
        if shown >= args.limit:
            break
        if not ERROR_LINE.search(line) or NOISE.match(line):
            continue
        number = f":{index + 1}" if args.log else ""
        location = f" file={args.log}{number}" if args.log else ""
        print(f"::error title=build{location}::{escape(line.strip())}")
        if args.context:
            for extra in lines[max(0, index - args.context):index]:
                print(f"  note: {extra.strip()[:200]}")
        shown += 1
    if not shown:
        tail = "\n".join(lines[-12:])[:900]
        print(f"::error title=build (no error line matched)::{escape(tail)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
