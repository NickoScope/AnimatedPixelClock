"""The registries: a name -> thing table with a decorator, nothing more.

Every extension point of the package is one of these tables. Adding a feature,
a provider, a portfolio mode, a metric, a window preset or an exchange means
registering one function or class; nothing else changes.

    from .registry import FEATURES
    @FEATURES.register("alerts")
    class AlertsFeature(Feature): ...

The tables are filled by the modules that define the entries, so importing
`matrix_market.app` (which imports every feature module) fills them all; the
CLI and the tests import the same modules.
"""
from typing import Callable, Dict, Generic, Iterator, List, Optional, TypeVar

T = TypeVar("T")


class Registry(Generic[T]):
    def __init__(self, kind: str):
        self.kind = kind
        self._items: Dict[str, T] = {}
        self._order: List[str] = []

    def register(self, name: str, item: Optional[T] = None):
        """`REG.register("x", obj)` or `@REG.register("x")` on a class/function."""
        if item is not None:
            self._add(name, item)
            return item

        def deco(obj: T) -> T:
            self._add(name, obj)
            return obj
        return deco

    def _add(self, name: str, item: T) -> None:
        if name in self._items and self._items[name] is not item:
            raise ValueError("%s %r is registered twice" % (self.kind, name))
        if name not in self._items:
            self._order.append(name)
        self._items[name] = item

    def unregister(self, name: str) -> None:
        """For tests and hot reloads; a name that is not there is not an error."""
        self._items.pop(name, None)
        if name in self._order:
            self._order.remove(name)

    def get(self, name: str) -> T:
        try:
            return self._items[name]
        except KeyError:
            raise KeyError("unknown %s %r; known: %s" % (self.kind, name, ", ".join(self._order) or "none")) from None

    def __contains__(self, name: object) -> bool:
        return name in self._items

    def __iter__(self) -> Iterator[str]:
        return iter(list(self._order))

    def items(self):
        return [(n, self._items[n]) for n in self._order]

    def names(self) -> List[str]:
        return list(self._order)


# The extension points. Filled by the modules named in each comment.
FEATURES: "Registry[type]" = Registry("feature")       # history, portfolio, live, tape, ha_entities (their modules)
PROVIDERS: "Registry[type]" = Registry("provider")     # providers/yahoo.py, providers/fixtures.py
MODES: "Registry[Callable]" = Registry("mode")         # ledger.py: hold, rebal
METRICS: "Registry[Callable]" = Registry("metric")     # windows.py: figures added to every windowed payload
WINDOWS: "Registry[Callable]" = Registry("window")     # windows.py: YTD 1Y 3Y 5Y 10Y MAX
LINES: "Registry[Callable]" = Registry("line")         # portfolio.py: pts px bench gross
EXCHANGES: "Registry[dict]" = Registry("exchange")     # tape.py: NYSE NASDAQ LSE XETRA EURONEXT TOKYO
TIMELINES: "Registry[Callable]" = Registry("timeline") # timeline.py: union, calendar
