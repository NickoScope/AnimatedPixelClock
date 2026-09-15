"""The line encoding on the wire: 128 uint16 values between `min` and `max`, little-endian, base64.

The packer is market_ref.pack_pts (the maths' own, so the previews and the app
agree byte for byte); this module adds the decoder the panel implements, so the
tests round-trip a payload the way the firmware will read it.
"""
import base64
import struct
from typing import List, Optional, Sequence, Tuple

from . import _ref

M = _ref.market_ref
POINTS = M.POINTS                     # 128
U16_MAX = 65535


def pack(points: Sequence[float], lo: Optional[float] = None, hi: Optional[float] = None) -> Tuple[float, float, str]:
    """(min, max, base64) for the payload. A flat line sits mid-scale (market_ref's rule)."""
    return M.pack_pts(list(points), lo, hi)


def unpack(b64: str) -> List[int]:
    """The raw uint16 values."""
    return M.unpack_pts(b64)


def decode(b64: str, lo: float, hi: float) -> List[float]:
    """What the panel reconstructs: min + u / 65535 x (max - min). Exact at both ends."""
    if hi <= lo:
        return [lo] * len(unpack(b64))
    return [lo + u * (hi - lo) / U16_MAX for u in unpack(b64)]


def check(b64: str, n: int = POINTS) -> None:
    """Refuse a line the panel could not draw: wrong length or bad base64."""
    try:
        raw = base64.b64decode(b64, validate=True)
    except (ValueError, TypeError) as e:
        raise ValueError("pts is not base64: %s" % type(e).__name__) from None
    if len(raw) != 2 * n:
        raise ValueError("pts holds %d bytes, the panel expects %d" % (len(raw), 2 * n))
    struct.unpack("<%dH" % n, raw)


def max_error(points: Sequence[float], lo: float, hi: float) -> float:
    """The largest reconstruction error of packing `points`: at most (max - min) / 65535 / 2."""
    _, _, b64 = pack(points, lo, hi)
    back = decode(b64, lo, hi)
    return max(abs(a - b) for a, b in zip(points, back)) if points else 0.0
