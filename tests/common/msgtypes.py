"""Message type constants sourced from src/net/message_types.h.

Populated at import time by the parser in msg_coverage. Tests should import
the symbols directly (e.g. `from tests.common.msgtypes import
STATESERVER_OBJECT_SET_FIELD`) — never hard-code values, since the parser
keeps this file in lock-step with the header.
"""
from __future__ import annotations

from .msg_coverage import ALL_MESSAGES

# Flatten every enum into module globals. Legacy client symbols are excluded
# — tests do not target ARDOS_USE_LEGACY_CLIENT builds.
_globals = globals()
for _enum, _syms in ALL_MESSAGES.items():
    if _enum.endswith("_LEGACY"):
        continue
    for _name, _val in _syms.items():
        _globals[_name] = _val

# Explicit aliases for the control-channel helpers — these are constants
# defined outside the enums in the header.
CONTROL_CHANNEL = 1
INVALID_CHANNEL = 0
INVALID_DO_ID = 0
BCHAN_CLIENTS = 10
BCHAN_STATESERVERS = 12
BCHAN_DBSERVERS = 13

del _globals, _enum, _syms, _name, _val
