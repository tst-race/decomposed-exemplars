#!/usr/bin/env python3
"""Regenerates actions.json for the file-based user model.

The timeline is relative to when the user model starts, i.e. when the plugin is
loaded. Anything that waits before it needs the link (for example a listener
that has been up for a minute before a bootstrap dialer connects) runs off the
end of a short timeline and silently stops fetching and posting, so the file
must cover the longest scenario.

Usage: generate_actions.py [duration_seconds] [output_path]
"""

import json
import sys
from pathlib import Path

DEFAULT_DURATION_S = 600


def main() -> None:
    duration_s = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_DURATION_S
    output = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).with_name("actions.json")

    # one action per second, alternating fetch (even seconds) and post (odd seconds)
    entries = [
        {
            "timestamp": float(second),
            "relative": True,
            "json": {"linkId": "*", "type": "fetch" if second % 2 == 0 else "post"},
        }
        for second in range(duration_s + 1)
    ]
    lines = [
        "    " + json.dumps(entry, separators=(", ", ": ")) for entry in entries
    ]
    output.write_text("[\n" + ",\n".join(lines) + "\n]\n")


if __name__ == "__main__":
    main()
