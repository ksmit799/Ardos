"""Parse src/net/message_types.h and track which messages tests exercise.

The header is the single source of truth for Ardos message codes. Parsing it
at test-collection time means the harness can't drift from the server — if a
new message type is added, tests start seeing it in the "not exercised" list
immediately.
"""

from __future__ import annotations

import re
from collections import defaultdict
from pathlib import Path
from typing import Dict, Set

_HEADER = Path(__file__).resolve().parents[2] / "src" / "net" / "message_types.h"

_ENUM_LINE = re.compile(r"^\s*([A-Z][A-Z0-9_]+)\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*,")
_ENUM_BLOCK_START = re.compile(r"^\s*enum\s+(\w+)")
_SKIP_IFDEF = re.compile(r"^\s*#ifdef\s+ARDOS_USE_LEGACY_CLIENT")
_ELSE = re.compile(r"^\s*#else")
_ENDIF = re.compile(r"^\s*#endif")


def _parse() -> Dict[str, Dict[str, int]]:
    """Return {enum_name: {symbol: value}} parsed from message_types.h.

    The header gates one ifdef-branch we don't target; the parser drops it so
    only the active arm's symbols populate the table.
    """
    out: Dict[str, Dict[str, int]] = defaultdict(dict)
    enum: str | None = None
    skip = False  # set while inside the skipped ifdef arm

    for line in _HEADER.read_text().splitlines():
        if _SKIP_IFDEF.search(line):
            skip = True
            continue
        if _ELSE.search(line) and skip:
            skip = False
            continue
        if _ENDIF.search(line):
            skip = False
            continue
        if skip:
            continue

        m = _ENUM_BLOCK_START.match(line)
        if m:
            enum = m.group(1)
            continue

        m = _ENUM_LINE.match(line)
        if m and enum is not None:
            sym, val = m.group(1), m.group(2)
            out[enum][sym] = int(val, 16) if val.startswith("0x") else int(val)

    return dict(out)


ALL_MESSAGES: Dict[str, Dict[str, int]] = _parse()

# Flat value -> symbol lookup across every parsed enum.
_FLAT: Dict[int, str] = {}
for enum_name, syms in ALL_MESSAGES.items():
    for sym, val in syms.items():
        _FLAT.setdefault(val, f"{enum_name}.{sym}")


def symbol_for(value: int) -> str:
    """Human-readable name for a message value, or hex if unknown."""
    return _FLAT.get(value, f"0x{value:04x}")


class CoverageTracker:
    """Records which message values were sent and received during a test run."""

    def __init__(self) -> None:
        self.sent: Set[int] = set()
        self.recv: Set[int] = set()

    def record_sent(self, msgtype: int) -> None:
        self.sent.add(int(msgtype))

    def record_recv(self, msgtype: int) -> None:
        self.recv.add(int(msgtype))

    def report(self) -> str:
        lines = ["", "=== Ardos message coverage ==="]
        for enum_name, syms in ALL_MESSAGES.items():
            if not syms:
                continue
            total = len(syms)
            exercised = {
                sym for sym, v in syms.items() if v in self.sent or v in self.recv
            }
            pct = (len(exercised) * 100 // total) if total else 0
            lines.append(f"  {enum_name}: {len(exercised)}/{total} ({pct}%)")
            missing = sorted(set(syms) - exercised)
            if missing:
                lines.append(f"    missing: {', '.join(missing)}")
        lines.append("")
        return "\n".join(lines)

    def ratio(self) -> float:
        """Overall coverage ratio across every parsed enum."""
        total = 0
        exercised = 0
        for _, syms in ALL_MESSAGES.items():
            total += len(syms)
            exercised += sum(
                1 for v in syms.values() if v in self.sent or v in self.recv
            )
        return exercised / total if total else 1.0


tracker = CoverageTracker()
