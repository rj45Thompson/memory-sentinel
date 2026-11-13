"""Memory Sentinel command line: `python ms.py scan|leaktest|heap|report|selftest ...` (UC-5). See `python ms.py -h`."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from memsentinel.cli import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
