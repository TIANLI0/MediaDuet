"""The logo has a single source for every platform: tools/make_icons.py at the
repository root. This wrapper only forwards to it so both apps stay identical.

Run: python -I windows/ui/tools/make_icon.py
"""
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).resolve().parents[3] / "tools" / "make_icons.py"), run_name="__main__")
