"""DC file handling for the test harness.

We use Panda3D's libdclass (via the `panda3d` pip package) so the DC hash the
harness uses to authenticate matches exactly what ardos computes at boot.
"""
from __future__ import annotations

from functools import lru_cache
from pathlib import Path

DC_DIR = Path(__file__).resolve().parents[1] / "files"


@lru_cache(maxsize=4)
def dc_hash(dc_filename: str = "test.dc") -> int:
    """Compute the DC hash for a file in tests/files/.

    The value returned matches `DCFile::get_hash()` on the server side.
    """
    try:
        from panda3d.direct import DCFile  # type: ignore
    except ImportError as e:  # pragma: no cover
        raise RuntimeError(
            "panda3d is required for DC hash computation. "
            "Install it via `pip install -r tests/requirements-test.txt`."
        ) from e

    dcf = DCFile()
    path = DC_DIR / dc_filename
    if not dcf.read(str(path)):
        raise RuntimeError(f"failed to parse {path}")
    return dcf.get_hash()


@lru_cache(maxsize=4)
def class_id(dc_filename: str, class_name: str) -> int:
    """Resolve a DC class name to its numeric index (what ardos sends)."""
    from panda3d.direct import DCFile  # type: ignore

    dcf = DCFile()
    path = DC_DIR / dc_filename
    dcf.read(str(path))
    cls = dcf.get_class_by_name(class_name)
    if cls is None:
        raise KeyError(f"class {class_name!r} not found in {dc_filename}")
    return cls.get_number()


@lru_cache(maxsize=64)
def field_id(dc_filename: str, class_name: str, field_name: str) -> int:
    """Resolve a DC field name to its numeric ID (for OBJECT_SET_FIELD)."""
    from panda3d.direct import DCFile  # type: ignore

    dcf = DCFile()
    path = DC_DIR / dc_filename
    dcf.read(str(path))
    cls = dcf.get_class_by_name(class_name)
    if cls is None:
        raise KeyError(f"class {class_name!r} not found in {dc_filename}")
    field = cls.get_field_by_name(field_name)
    if field is None:
        raise KeyError(f"field {field_name!r} not found on {class_name}")
    return field.get_number()
