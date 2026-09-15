"""MQTT: the topics under <base>/<device>/market/, retained payloads, the size guard, generations.

Topics (docs/18, "MQTT contract"): status, index/<sym>/<preset>, ticker/<sym>/<preset>,
portfolio/<mode>/<preset>, holdings/<mode>, live, intraday/<sym>, tape, ha (the will);
config comes from the panel.

The bus buffer on the panel is 2048 B with the topic and header in it, so every
payload is kept under PAYLOAD_MAX = 1900 B (the media app's figure). A payload
that does not fit is shrunk by the rule for its topic (the design: "drops the
optional lines first"; live drops the day high/low first) and refused, with a
log line, when nothing more can go.

A generation is one consistent set: every payload of a refresh carries the same
`gen`, `status` goes last with that `gen`, and a retained topic of the previous
generation that the new one does not carry is cleared (an empty retained
message), so a symbol removed in the portal leaves no stale page behind.
"""
import json
import time
from typing import Callable, Dict, Iterable, List, Optional, Set, Tuple

from . import SCHEMA_V

PAYLOAD_MAX = 1900
CONFIG_MAX = 8192          # the panel's config payload: 16 positions and the lists fit in this


def encode(payload: dict) -> bytes:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")


class Topics:
    """market/<leaf> for the panel; stats/<mode>/<preset> goes to market_stats/<mode>/<preset>, outside the
    panel's market/# subscription, where it would count every message as an unknown topic (audit MINOR 1)."""
    STATS_PREFIX = "stats/"

    def __init__(self, base: str, device: str):
        self.root = "%s/%s/market/" % (base, device)
        self.stats_root = "%s/%s/market_stats/" % (base, device)

    def leaf(self, name: str) -> str:
        if name.startswith(self.STATS_PREFIX):
            return self.stats_root + name[len(self.STATS_PREFIX):]
        return self.root + name

    def name(self, topic: str) -> Optional[str]:
        return topic[len(self.root):] if topic.startswith(self.root) else None

    @property
    def config(self) -> str:
        return self.leaf("config")

    @property
    def ha(self) -> str:
        return self.leaf("ha")

    @property
    def wildcard(self) -> str:
        return self.root + "#"


# ── shrinking rules by topic ─────────────────────────────────────────────────
def _drop_keys(payload: dict, keys: Iterable[str]) -> bool:
    for k in keys:
        if k in payload:
            del payload[k]
            return True
    return False


def shrink_portfolio(p: dict) -> bool:
    return _drop_keys(p, ("gross", "tell", "bench"))       # never px: the panel requires it (audit M6)


def shrink_live(p: dict) -> bool:
    """The design's rule: every day high/low goes first, then quotes from the end of the list."""
    q = p.get("q") or {}
    dropped = False
    for row in q.values():
        if isinstance(row, dict) and ("hi" in row or "lo" in row):
            row.pop("hi", None)
            row.pop("lo", None)
            dropped = True
    if dropped:
        return True
    for row in q.values():
        if isinstance(row, dict) and "delay_s" in row:
            for r in q.values():
                if isinstance(r, dict):
                    r.pop("delay_s", None)
            return True
    if q:
        q.pop(next(reversed(list(q))))
        return True
    return False


def shrink_status(p: dict) -> bool:
    if p.get("cfgErr"):
        p["cfgErr"] = p["cfgErr"][:-1]
        return True
    syms = p.get("symbols") or {}
    for row in syms.values():
        if isinstance(row, dict) and "terSrc" in row:
            del row["terSrc"]
            return True
    if syms:
        syms.pop(next(reversed(list(syms))))
        return True
    return False


def shrink_rows(key: str, fields: Tuple[str, ...] = ("cls", "lo", "hi", "entry")) -> Callable[[dict], bool]:
    """Optional row fields first (each field from every row), then rows from the end."""
    def shrink(p: dict) -> bool:
        rows = p.get(key)
        if not isinstance(rows, list) or not rows:
            return False
        for f in fields:
            if any(isinstance(r, dict) and f in r for r in rows):
                for r in rows:
                    if isinstance(r, dict):
                        r.pop(f, None)
                return True
        rows.pop()
        return True
    return shrink


def shrink_stats(p: dict) -> bool:
    for key in ("topDD", "contrib", "roll3y", "roll1y", "bench", "classes"):
        if key in p:
            del p[key]
            return True
    return False


def shrink_live_extra(p: dict) -> bool:
    """Before the day high/low: nothing; the live rule chains: hi/lo, then delay_s, then quotes."""
    return False


SHRINK: List[Tuple[str, Callable[[dict], bool]]] = [
    ("portfolio/", shrink_portfolio),
    ("stats/", shrink_stats),
    ("live", shrink_live),
    ("status", shrink_status),
    ("holdings/", shrink_rows("rows")),
    ("tape", shrink_rows("x", ())),
]


def fit(leaf: str, payload: dict, max_bytes: int = PAYLOAD_MAX) -> Tuple[Optional[bytes], List[str]]:
    """The encoded payload under max_bytes, shrunk by its topic's rule; (None, notes) when it cannot."""
    notes: List[str] = []
    data = encode(payload)
    rule = next((fn for prefix, fn in SHRINK if leaf.startswith(prefix)), None)
    while len(data) > max_bytes:
        if rule is None or not rule(payload):
            return None, notes + ["%s: %d B does not fit %d" % (leaf, len(data), max_bytes)]
        notes.append("%s: shrunk" % leaf)
        data = encode(payload)
    return data, notes


# ── generations ──────────────────────────────────────────────────────────────
class Snapshot:
    """One generation's payloads, in publish order."""

    def __init__(self, gen: int, asof: Optional[str]):
        self.gen, self.asof = gen, asof
        self.payloads: Dict[str, dict] = {}
        self.notes: List[str] = []

    def add(self, leaf: str, payload: dict) -> None:
        payload.setdefault("v", SCHEMA_V)
        payload["gen"] = self.gen
        self.payloads[leaf] = payload

    def leaves(self) -> List[str]:
        return list(self.payloads)


class Publisher:
    LEAVES_TABLE = "published_leaves"

    def __init__(self, client, topics: Topics, log: Callable[..., None] = lambda *a, **k: None, max_bytes: int = PAYLOAD_MAX,
                 clock: Callable[[], float] = time.time, store=None):
        self.client, self.topics, self.log, self.max_bytes, self.clock = client, topics, log, max_bytes, clock
        self.store = store
        # the retained topics of the last generation, kept in the store, so a topic dropped while the app was down is
        # still cleared by the first generation after the restart (audit MINOR 2)
        saved = store.load_table(self.LEAVES_TABLE) if store is not None else None
        self.generation_leaves: Set[str] = set(saved.get("leaves", [])) if isinstance(saved, dict) else set()
        self.sizes: Dict[str, int] = {}
        self.refused: List[str] = []
        self._gen = 0

    def new_snapshot(self, asof: Optional[str]) -> Snapshot:
        self._gen = max(self._gen + 1, int(self.clock()))    # increasing across restarts too
        return Snapshot(self._gen, asof)

    def publish(self, leaf: str, payload: dict, retain: bool = True, qos: int = 0) -> bool:
        if isinstance(payload, dict):
            payload.setdefault("v", SCHEMA_V)      # every payload, the direct ones too (tape, live, intraday): the panel refuses one without v
        data, notes = fit(leaf, payload, self.max_bytes)
        for n in notes:
            self.log("market: %s" % n, level="WARNING")
        if data is None:
            self.refused.append(leaf)
            return False
        self.client.publish(self.topics.leaf(leaf), data, qos=qos, retain=retain)
        self.sizes[leaf] = len(data)
        return True

    def clear(self, leaf: str) -> None:
        self.client.publish(self.topics.leaf(leaf), b"", qos=0, retain=True)
        self.sizes.pop(leaf, None)
        if leaf in self.generation_leaves:
            self.generation_leaves.discard(leaf)
            self._save_leaves()

    def _save_leaves(self) -> None:
        if self.store is not None:
            self.store.save_table(self.LEAVES_TABLE, {"v": 1, "leaves": sorted(self.generation_leaves)})

    def publish_snapshot(self, snap: Snapshot, status: dict, keep_previous: bool = False) -> Dict[str, int]:
        """Every payload, then the topics of the previous generation this one lacks are cleared (unless
        `keep_previous`: a feature failed, its records stay), then `status` with the generation. Returns the sizes."""
        sent: Set[str] = set()
        for leaf, payload in snap.payloads.items():
            if self.publish(leaf, payload):
                sent.add(leaf)
        if keep_previous:
            sent |= self.generation_leaves
        else:
            for leaf in sorted(self.generation_leaves - sent):
                self.clear(leaf)
        self.generation_leaves = sent
        self._save_leaves()
        status["gen"] = snap.gen
        status.setdefault("v", SCHEMA_V)
        if self.refused:
            status["refused"] = sorted(set(self.refused))[:8]
            self.refused = []
        self.publish("status", status)
        return dict(self.sizes)

    def online(self, up: bool) -> None:
        self.client.publish(self.topics.ha, "online" if up else "offline", qos=1, retain=True)


class DryRunClient:
    """A paho stand-in that records; the CLI prints what it recorded."""

    def __init__(self):
        self.published: List[Tuple[str, bytes, int, bool]] = []

    def publish(self, topic, payload=None, qos=0, retain=False):
        self.published.append((topic, payload if isinstance(payload, bytes) else str(payload).encode(), qos, retain))
