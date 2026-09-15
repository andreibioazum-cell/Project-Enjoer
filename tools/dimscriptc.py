#!/usr/bin/env python3
"""Repository-friendly entry point for the DimScript compiler."""

from pathlib import Path
import sys

# Make ``python3 tools/dimscriptc.py`` work from any current directory.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from dimscript.cli import main  # noqa: E402

raise SystemExit(main())
