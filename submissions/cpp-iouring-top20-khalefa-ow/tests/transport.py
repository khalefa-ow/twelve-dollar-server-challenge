#!/usr/bin/env python3
"""Run the shared io_uring transport checks against this variant's binary."""
from pathlib import Path
import runpy

HERE = Path(__file__).resolve().parent.parent
BASE = HERE.parent / "cpp-iouring-khalefa-ow" / "tests" / "transport.py"
suite = runpy.run_path(BASE, run_name="_shared_transport")
suite["HERE"] = HERE
suite["main"]()
