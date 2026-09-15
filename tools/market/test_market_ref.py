#!/usr/bin/env python3
"""Unit tests of the reference maths, with the arithmetic done by hand in the comments.

  python3 -m unittest tools/market/test_market_ref.py -v
"""
import datetime as dt
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import market_ref as M  # noqa: E402

D = dt.date


def mk(sym, cur, bars, divs=(), ter=None):
    """A Series from ("YYYY-MM-DD", close) pairs."""
    return M.Series(sym, cur, [D.fromisoformat(d) for d, _ in bars], [float(c) for _, c in bars],
                    [(D.fromisoformat(d), float(a)) for d, a in divs], ter)


def cfg(positions, capital=10000.0, currency="EUR", inception="2023-01-02", **kw):
    ps = [M.Position(p[0], float(p[1]), D.fromisoformat(p[2]) if len(p) > 2 and p[2] else None) for p in positions]
    return M.Config(capital, currency, D.fromisoformat(inception), ps, **kw)


FLAT_FX = M.FxTable([D(2000, 1, 1)], [1.0])    # EUR-only cases never look at it


def run(c, series, mode, days=None, dividends=True, fx=FLAT_FX):
    series = {s.sym: s for s in series}
    days = days or M.timeline(list(series.values()), c.inception)
    return M.simulate(c, series, fx, days, mode, dividends)


class TwoAssetDrift(unittest.TestCase):
    """A rises 100 -> 120 by the first year-end and to 150 after; B is flat at 100.

    Start 2023-01-02, 10 000 EUR, 50/50: qtyA = 5000/100 = 50, qtyB = 50, cash 0.
    Year-end 2023-12-29: A = 120 -> V = 50*120 + 50*100 = 11 000.
      HOLD: no cash, nothing to buy. End 2024-12-31: 50*150 + 50*100 = 12 500.
      REBAL: A -> 5 500/120 = 45.8333 sh, B -> 5 500/100 = 55 sh.
             2024-06-03: 45.8333*140 + 55*100 = 6 416.67 + 5 500 = 11 916.67.
             End 2024-12-31: 45.8333*150 + 55*100 = 6 875 + 5 500 = 12 375; that day is a
             year-end too, so REBAL rebalances again at the close: A -> 6 187.5/150 = 41.25 sh,
             B -> 61.875 sh, V unchanged.
    HOLD - REBAL = 125: rebalancing sold the winner.
    """
    A = mk("A", "EUR", [("2023-01-02", 100), ("2023-06-01", 110), ("2023-12-29", 120), ("2024-06-03", 140), ("2024-12-31", 150)])
    B = mk("B", "EUR", [("2023-01-02", 100), ("2023-06-01", 100), ("2023-12-29", 100), ("2024-06-03", 100), ("2024-12-31", 100)])
    C = cfg([("A", 50), ("B", 50)])

    def test_hold(self):
        L = run(self.C, [self.A, self.B], "hold")
        self.assertEqual(L.qty, {"A": 50.0, "B": 50.0})
        self.assertAlmostEqual(L.v[-1], 12500.0)
        self.assertAlmostEqual(L.cash[-1], 0.0)

    def test_rebal(self):
        L = run(self.C, [self.A, self.B], "rebal")
        self.assertAlmostEqual(L.v[L.index_on(D(2023, 12, 29))], 11000.0)   # the rebalance keeps V
        self.assertAlmostEqual(L.v[L.index_on(D(2024, 6, 3))], 5500 / 120 * 140 + 5500)
        self.assertAlmostEqual(L.v[-1], 12375.0)
        self.assertAlmostEqual(L.qty["A"], 6187.5 / 150)
        self.assertAlmostEqual(L.qty["B"], 61.875)

    def test_difference_is_125(self):
        H = run(self.C, [self.A, self.B], "hold")
        R = run(self.C, [self.A, self.B], "rebal")
        self.assertAlmostEqual(H.v[-1] - R.v[-1], 125.0)


class LateListing(unittest.TestCase):
    """B's fund starts trading 2023-06-01; its 5 000 sits in cash until the first year-end
    after that, 2023-12-29 (owner, 2026-09-15 09:23).

    Start 2022-01-03: A = 100 -> 50 sh (5 000); B reserved 5 000; cash 5 000.
    Year-end 2022-12-30, A = 110, B not trading:
      HOLD: free cash = 5 000 - reserved 5 000 = 0 -> nothing bought; cash stays 5 000.
      REBAL: V = 5 500 + 5 000 = 10 500; A -> 5 250 (47.7273 sh); cash = 0.5 * V = 5 250.
    2023-06-01: B lists at 200. Nothing happens until the year-end.
    Year-end 2023-12-29, A = 110, B = 200:
      HOLD: buy B with its reserved 5 000 -> 25 sh; free cash 0. V = 5 500 + 5 000 = 10 500.
      REBAL: V = 47.7273*110 + 5 250 = 10 500; A -> 5 250 (47.7273 sh), B -> 5 250/200 = 26.25 sh; cash 0.
    """
    A = mk("A", "EUR", [("2022-01-03", 100), ("2022-06-01", 105), ("2022-12-30", 110), ("2023-06-01", 110), ("2023-12-29", 110), ("2024-01-03", 110)])
    B = mk("B", "EUR", [("2023-06-01", 200), ("2023-12-29", 200), ("2024-01-03", 200)])
    C = cfg([("A", 50), ("B", 50)], inception="2022-01-03")

    def test_hold(self):
        L = run(self.C, [self.A, self.B], "hold")
        i_list = L.index_on(D(2023, 6, 1))
        self.assertAlmostEqual(L.cash[i_list], 5000.0)                   # reserved, still cash after listing
        self.assertAlmostEqual(L.cash[L.index_on(D(2022, 12, 30))], 5000.0)
        self.assertEqual(L.entries["B"], D(2023, 12, 29))
        self.assertAlmostEqual(L.qty["A"], 50.0)                          # never topped up: no free cash
        self.assertAlmostEqual(L.qty["B"], 25.0)
        self.assertAlmostEqual(L.cash[-1], 0.0)
        self.assertAlmostEqual(L.v[-1], 10500.0)

    def test_rebal(self):
        L = run(self.C, [self.A, self.B], "rebal")
        self.assertAlmostEqual(L.cash[L.index_on(D(2022, 12, 30))], 5250.0)
        self.assertAlmostEqual(L.qty["A"], 5250 / 110)
        self.assertEqual(L.entries["B"], D(2023, 12, 29))
        self.assertAlmostEqual(L.qty["B"], 26.25)
        self.assertAlmostEqual(L.cash[-1], 0.0)
        self.assertAlmostEqual(L.v[-1], 10500.0)

    def test_holdings_rows(self):
        L = run(self.C, [self.A, self.B], "hold")
        h = M.holdings_payload(self.C, L, {"A": self.A, "B": self.B}, FLAT_FX)
        now = {r["sym"]: r["now"] for r in h["rows"]}
        self.assertAlmostEqual(now["A"], 100 * 5500 / 10500)
        self.assertAlmostEqual(now["B"], 100 * 5000 / 10500)
        self.assertAlmostEqual(sum(now.values()) + h["cash"]["now"], 100.0)
        self.assertAlmostEqual(next(r["ret"] for r in h["rows"] if r["sym"] == "A"), 0.10)   # 100 -> 110


class FxBothWays(unittest.TestCase):
    """EURUSD=X is USD per EUR: 1.25 at the start, 1.00 at the end (the dollar strengthens).

    EUR portfolio, a USD asset flat at 100: entry price 100 / 1.25 = 80 EUR -> 125 sh;
      end 125 * 100 / 1.00 = 12 500 EUR, +25 %.
    USD portfolio, a EUR asset flat at 100: entry price 100 * 1.25 = 125 USD -> 80 sh;
      end 80 * 100 * 1.00 = 8 000 USD, -20 %.
    """
    FX = M.FxTable([D(2023, 1, 2), D(2023, 12, 29)], [1.25, 1.00])
    U = mk("U", "USD", [("2023-01-02", 100), ("2023-12-29", 100)])
    E = mk("E", "EUR", [("2023-01-02", 100), ("2023-12-29", 100)])

    def test_usd_asset_in_eur(self):
        L = run(cfg([("U", 100)], currency="EUR"), [self.U], "hold", fx=self.FX)
        self.assertAlmostEqual(L.qty["U"], 125.0)
        self.assertAlmostEqual(L.v[-1], 12500.0)

    def test_eur_asset_in_usd(self):
        L = run(cfg([("E", 100)], currency="USD"), [self.E], "hold", fx=self.FX)
        self.assertAlmostEqual(L.qty["E"], 80.0)
        self.assertAlmostEqual(L.v[-1], 8000.0)

    def test_factor_direction(self):
        self.assertAlmostEqual(self.FX.factor("USD", "EUR", D(2023, 3, 1)), 0.8)    # carried 1.25
        self.assertAlmostEqual(self.FX.factor("EUR", "USD", D(2023, 12, 29)), 1.0)
        self.assertEqual(self.FX.factor("EUR", "EUR", D(2023, 1, 2)), 1.0)

    def test_refuses_before_the_series(self):
        """A conversion before 2003-12-01 goes to the ECB hook, which is a stub: refuse."""
        fx = M.FxTable([D(2003, 12, 1)], [1.2])
        self.assertIsNone(M.ecb_monthly_usd_per_eur(2001, 6))
        with self.assertRaises(M.NoFxRate) as cm:
            fx.factor("USD", "EUR", D(2003, 6, 2))
        self.assertIn("2003-06-02", str(cm.exception))
        self.assertIn("ECB", str(cm.exception))
        early = mk("U", "USD", [("2003-06-02", 100), ("2003-12-01", 100)])
        with self.assertRaises(M.NoFxRate):
            run(cfg([("U", 100)], inception="2003-06-02"), [early], "hold", fx=fx)


class DividendOnRebalanceDay(unittest.TestCase):
    """A at 100, 100 sh. On the year-end 2023-12-29 a 1.00 dividend lands: cash 100, DIV 100.
    The year-end then puts that cash to work: 100 / 100 = 1 more share, 101 in both modes.
    The no-dividend run keeps 100 shares and 10 000. (A 2024 bar follows, so the ledger
    knows 2023-12-29 was the year's last day.)"""
    A = mk("A", "EUR", [("2023-01-02", 100), ("2023-12-29", 100), ("2024-01-03", 100)], divs=[("2023-12-29", 1.0)])
    C = cfg([("A", 100)])

    def test_both_modes(self):
        for mode in ("hold", "rebal"):
            L = run(self.C, [self.A], mode)
            self.assertAlmostEqual(L.qty["A"], 101.0, msg=mode)
            self.assertAlmostEqual(L.cash[-1], 0.0, msg=mode)
            self.assertAlmostEqual(L.div[-1], 100.0, msg=mode)
            self.assertAlmostEqual(L.v[-1], 10100.0, msg=mode)

    def test_no_dividend_run(self):
        L = run(self.C, [self.A], "hold", dividends=False)
        self.assertAlmostEqual(L.qty["A"], 100.0)
        self.assertAlmostEqual(L.div[-1], 0.0)
        self.assertAlmostEqual(L.v[-1], 10000.0)

    def test_dividend_before_entry_is_not_paid(self):
        A = mk("A", "EUR", [("2023-01-02", 100), ("2023-12-29", 100), ("2024-01-03", 100)], divs=[("2022-12-15", 5.0)])
        L = run(self.C, [A], "hold")
        self.assertAlmostEqual(L.div[-1], 0.0)


class Contribution(unittest.TestCase):
    """1 000 EUR every year on the inception's anniversary, 2024-01-02. It sits in cash
    (V = 10 000 + 1 000 on 2024-06-03) until the year-end 2024-12-31, then buys
    10 shares at 100: 110 sh. sinceStart = 11 000 / (10 000 + 1 000) - 1 = 0."""
    A = mk("A", "EUR", [("2023-01-02", 100), ("2023-07-03", 100), ("2024-01-02", 100), ("2024-06-03", 100), ("2024-12-31", 100)])
    C = cfg([("A", 100)], contrib_amount=1000.0, contrib_every="year")

    def test_contribution(self):
        L = run(self.C, [self.A], "hold")
        self.assertEqual(M.contribution_dates(self.C, D(2024, 12, 31)), [D(2024, 1, 2)])
        self.assertAlmostEqual(L.cash[L.index_on(D(2024, 6, 3))], 1000.0)
        self.assertAlmostEqual(L.v[L.index_on(D(2024, 6, 3))], 11000.0)
        self.assertAlmostEqual(L.qty["A"], 110.0)
        self.assertAlmostEqual(L.contrib[-1], 1000.0)
        p = M.portfolio_payload(self.C, L, run(self.C, [self.A], "hold", dividends=False), None, "MAX")
        self.assertAlmostEqual(p["sinceStart"], 0.0)
        self.assertAlmostEqual(p["chg"], 0.10)          # V 10 000 -> 11 000 over the window

    def test_monthly_dates(self):
        c = cfg([("A", 100)], inception="2023-01-31", contrib_amount=1.0, contrib_every="month")
        self.assertEqual(M.contribution_dates(c, D(2023, 4, 30))[:3], [D(2023, 2, 28), D(2023, 3, 31), D(2023, 4, 30)])


class Windows(unittest.TestCase):
    """Bars: 2024-01-02 100, 2024-06-03 110, 2024-12-30 120, 2025-03-03 130, 2025-06-02 125, 2025-09-15 150."""
    S = mk("I", "USD", [("2024-01-02", 100), ("2024-06-03", 110), ("2024-12-30", 120), ("2025-03-03", 130), ("2025-06-02", 125), ("2025-09-15", 150)])
    ASOF, INC = D(2025, 9, 15), D(2020, 1, 1)

    def test_1y_shorter_than_history(self):
        """1Y starts 2024-09-15, no bar there: the value carried from 2024-06-03 (110) opens the window."""
        p = M.index_payload(self.S, "1Y", self.ASOF, self.INC)
        self.assertEqual((p["from"], p["to"]), ("2024-09-15", "2025-09-15"))
        self.assertAlmostEqual(p["chg"], 150 / 110 - 1)
        self.assertEqual((p["hi"], p["lo"]), (150.0, 110.0))
        self.assertAlmostEqual(p["mdd"], 125 / 130 - 1)
        self.assertIsNone(p["cagr"])

    def test_ytd(self):
        """YTD opens at the last bar of the previous year, 2024-12-30 = 120."""
        p = M.index_payload(self.S, "YTD", self.ASOF, self.INC)
        self.assertEqual(p["from"], "2024-12-30")
        self.assertAlmostEqual(p["chg"], 0.25)

    def test_max_short_history(self):
        """MAX is from the inception 2020, but the series starts 2024-01-02: the window starts there."""
        p = M.index_payload(self.S, "MAX", self.ASOF, self.INC)
        self.assertEqual(p["from"], "2024-01-02")
        self.assertAlmostEqual(p["chg"], 0.5)
        self.assertIsNone(p["cagr"])                     # under three years

    def test_cagr_when_long_enough(self):
        S = mk("J", "USD", [("2020-01-02", 100), ("2023-01-03", 200)])
        p = M.index_payload(S, "MAX", D(2023, 1, 3), D(2020, 1, 2))
        self.assertAlmostEqual(p["cagr"], 2 ** (1 / M.years_between(D(2020, 1, 2), D(2023, 1, 3))) - 1)

    def test_downsample(self):
        """Four bars ten days apart over 30 days: 3 slices end at days 10, 20, 30 -> the bars there;
        6 slices end at 5, 10, ... -> the bar carried into each empty slice."""
        wd = [D(2024, 1, 1), D(2024, 1, 11), D(2024, 1, 21), D(2024, 1, 31)]
        self.assertEqual(M.downsample(wd, [1, 2, 3, 4], 3), [2, 3, 4])
        self.assertEqual(M.downsample(wd, [1, 2, 3, 4], 6), [1, 2, 2, 3, 3, 4])
        pts = M.downsample(wd, [1, 2, 3, 4])
        self.assertEqual((len(pts), pts[0], pts[-1]), (128, 1, 4))

    def test_pack_unpack(self):
        lo, hi, b64 = M.pack_pts([1.0, 2.0, 3.0])
        self.assertEqual((lo, hi), (1.0, 3.0))
        self.assertEqual(M.unpack_pts(b64), [0, 32768, 65535])
        self.assertEqual(M.unpack_pts(M.pack_pts([5.0, 5.0])[2]), [32767, 32767])
        self.assertEqual(len(M.pack_pts([0.0] * 128)[2]), 344)   # 256 B in base64, as the design says


class TerDrag(unittest.TestCase):
    def test_daily_252(self):
        """253 weekday bars, value flat 10 000, TER 0.20 %: 252 steps of 10 000 x 0.002 / 252 = 20.00."""
        days, d = [], D(2024, 1, 1)
        while len(days) < 253:
            if d.weekday() < 5:
                days.append(d)
            d += dt.timedelta(days=1)
        A = M.Series("A", "EUR", days, [100.0] * len(days), [], 0.002)
        L = M.simulate(cfg([("A", 100)], inception="2024-01-01"), {"A": A}, FLAT_FX, days, "hold")
        self.assertAlmostEqual(L.ter[-1], 20.0)

    def test_calendar_bars(self):
        """Monthly bars pay a month's worth each: 31 days / 365.25 of 10 000 x 0.002."""
        A = mk("A", "EUR", [("2024-01-01", 100), ("2024-02-01", 100)], ter=0.002)
        L = M.simulate(cfg([("A", 100)], inception="2024-01-01"), {"A": A}, FLAT_FX, A.dates, "hold", True, M.calendar_years)
        self.assertAlmostEqual(L.ter[-1], 10000 * 0.002 * 31 / 365.25)


class Calendar(unittest.TestCase):
    def test_year_ends(self):
        days = [D(2023, 1, 2), D(2023, 12, 29), D(2024, 6, 3), D(2024, 9, 14)]
        self.assertEqual(M.year_ends(days), {D(2023, 12, 29)})          # 2024 is not over
        self.assertEqual(M.year_ends(days + [D(2024, 12, 31)]), {D(2023, 12, 29), D(2024, 12, 31)})

    def test_stale(self):
        self.assertFalse(M.is_stale(D(2026, 9, 14), D(2026, 9, 15)))    # Mon -> Tue, 1 trading day
        self.assertFalse(M.is_stale(D(2026, 9, 14), D(2026, 9, 17)))    # 3
        self.assertTrue(M.is_stale(D(2026, 9, 14), D(2026, 9, 18)))     # 4
        self.assertTrue(M.is_stale(D(2026, 9, 14), D(2026, 9, 21)))     # the weekend does not count

    def test_add_years(self):
        self.assertEqual(M.add_years(D(2024, 2, 29), -1), D(2023, 2, 28))
        self.assertEqual(M.window_start("5Y", D(2026, 9, 14), D(2000, 1, 1), []), D(2021, 9, 14))


class ExchangeState(unittest.TestCase):
    """NYSE as Yahoo's meta gives it: pre 04:00-09:30, regular 09:30-16:00, post 16:00-20:00 New York."""
    NY = {"gmtoffset": -14400, "pre": (4 * 3600, 34200), "regular": (34200, 57600), "post": (57600, 72000)}
    PAR = {"gmtoffset": 7200, "pre": (32400, 32400), "regular": (32400, 63000), "post": (63000, 63000)}

    def test_states(self):
        utc = dt.timezone.utc
        self.assertEqual(M.exchange_state(self.NY, dt.datetime(2026, 9, 14, 14, 30, tzinfo=utc)), "OPEN")    # 10:30 NY
        self.assertEqual(M.exchange_state(self.NY, dt.datetime(2026, 9, 14, 20, 30, tzinfo=utc)), "POST")    # 16:30
        self.assertEqual(M.exchange_state(self.NY, dt.datetime(2026, 9, 15, 6, 40, tzinfo=utc)), "CLOSED")   # 02:40
        self.assertEqual(M.exchange_state(self.NY, dt.datetime(2026, 9, 15, 8, 30, tzinfo=utc)), "PRE")      # 04:30
        self.assertEqual(M.exchange_state(self.NY, dt.datetime(2026, 9, 19, 14, 30, tzinfo=utc)), "CLOSED")  # Saturday
        self.assertEqual(M.exchange_state(self.PAR, dt.datetime(2026, 9, 15, 6, 40, tzinfo=utc)), "CLOSED")  # 08:40, no pre session

    def test_from_sample_meta(self):
        s = M.load_yahoo_chart(M.SAMPLES / "yahoo_VOO_max_1mo.json")
        ses = M.session_from_meta(s.meta)
        self.assertEqual(ses["regular"], (34200, 57600))
        self.assertEqual(ses["gmtoffset"], -14400)


class Samples(unittest.TestCase):
    """The preview portfolio on the saved answers: VOO alone, 100 %, 10 000 EUR from 2000-01-01.
    VOO's first bar is 2010-10-01, so the capital is reserved cash until the first year-end after
    it, 2010-12-01 in the monthly fixture."""
    @classmethod
    def setUpClass(cls):
        cls.P = M.run_preview_portfolio()

    def test_config_is_the_preview_portfolio_only(self):
        syms = [p.sym for p in self.P["cfg"].positions]
        self.assertTrue(set(syms) <= {s for s, _ in M.PREVIEW_PORTFOLIO})
        self.assertNotIn("IWDA.AS", syms)
        self.assertAlmostEqual(sum(p.w for p in self.P["cfg"].positions), 100.0, places=3)

    def test_entries_and_asof(self):
        for mode in ("hold", "rebal"):
            L = self.P["runs"][mode]
            self.assertEqual(L.entries["VOO"], D(2010, 12, 1))
            self.assertEqual(L.days[-1], D(2026, 9, 14))
            i = L.index_on(D(2010, 11, 30))
            self.assertTrue(all(abs(v - 10000.0) < 1e-9 for v in L.v[:i + 1]))     # all cash until VOO enters
            self.assertGreater(L.div[-1], 0)

    def test_parser_fixtures(self):
        """IWDA.AS: an EUR fund without dividends; AAPL: weekly bars with 20 dividends. Fixtures only."""
        iwda = M.load_yahoo_chart(M.SAMPLES / "yahoo_IWDAAS.json")
        aapl = M.load_yahoo_chart(M.SAMPLES / "yahoo_AAPL_5y_1wk.json")
        self.assertEqual((iwda.currency, iwda.dividends, iwda.first), ("EUR", [], D(2009, 9, 1)))
        self.assertEqual((aapl.currency, len(aapl.dividends), aapl.first), ("USD", 20, D(2021, 9, 13)))

    def test_split_adjusted_quantities(self):
        """VOO's 2013 1:2 reverse split is already in close[] and in the dividends: qty is never scaled."""
        s = self.P["samples"]["series"]["VOO"]
        self.assertGreater(s.close_on(D(2013, 10, 1)), 150)      # 160.88 pre-split, doubled in the data
        self.assertEqual(len(s.dividends), 63)

    def test_ter_from_samples(self):
        S = self.P["samples"]["series"]
        self.assertAlmostEqual(S["VOO"].ter, 0.0003, places=6)
        self.assertAlmostEqual(S["IWDA.AS"].ter, 0.002)       # the quoteSummary fixture parses
        self.assertIsNone(S["AAPL"].ter)                       # a stock: no expense ratio

    def test_fx_direction_on_a_real_bar(self):
        S, fx = self.P["samples"]["series"], self.P["samples"]["fx"]
        d = D(2026, 9, 14)
        self.assertAlmostEqual(S["VOO"].close_on(d) * fx.factor("USD", "EUR", d), 699.3 / fx.usd_per_eur(d), places=3)
        self.assertLess(fx.factor("USD", "EUR", d), 1.0)          # the dollar is worth less than a euro here

    def test_payload_shape(self):
        L, Ln = self.P["runs"]["hold"], self.P["runs"]["hold_nodiv"]
        p = M.portfolio_payload(self.P["cfg"], L, Ln, self.P["bench"], "5Y")
        for k in ("pts", "px", "bench"):
            self.assertEqual(len(M.unpack_pts(p[k])), 128)
        self.assertEqual(p["from"], "2021-09-14")
        self.assertGreater(p["div"], 0)
        self.assertGreaterEqual(p["value"], p["min"])


class EcbHook(unittest.TestCase):
    """The saved ECB table, EXR.M.USD.EUR.SP00.E for 1999-01..2004-12, is FxTable's `ecb` hook in the
    samples' FX: a month before Yahoo's first EURUSD=X bar (2003-12-01) is answered from the table,
    from that bar on Yahoo answers, and a month the table lacks still refuses. The default hook,
    without a table, is as empty as before."""
    T = M.load_ecb_monthly(M.SAMPLES / M.ECB_SAMPLE)

    def test_the_table(self):
        self.assertEqual(len(self.T), 72)
        self.assertEqual((min(self.T), max(self.T)), ((1999, 1), (2004, 12)))

    def test_refuses_the_average_series(self):
        """The same file with the key of the monthly average, EXR.M.USD.EUR.SP00.A, is refused."""
        import tempfile
        text = (M.SAMPLES / M.ECB_SAMPLE).read_text().replace("EXR.M.USD.EUR.SP00.E,", "EXR.M.USD.EUR.SP00.A,")
        with tempfile.TemporaryDirectory() as tmp:
            p = pathlib.Path(tmp) / "a.csv"
            p.write_text(text)
            with self.assertRaises(ValueError):
                M.load_ecb_monthly(p)

    def test_the_hook_in_the_sample_fx(self):
        fx = M.load_samples()["fx"]
        self.assertEqual(fx.usd_per_eur(D(2001, 6, 1)), self.T[(2001, 6)])
        self.assertEqual(fx.usd_per_eur(D(2003, 11, 3)), self.T[(2003, 11)])
        self.assertEqual(fx.usd_per_eur(D(2003, 12, 1)), fx.rates[0])          # Yahoo from its first bar
        with self.assertRaises(M.NoFxRate):
            fx.usd_per_eur(D(1998, 12, 1))
        self.assertIsNone(M.ecb_monthly_usd_per_eur(2001, 6))


class BenchmarkInPortfolioCurrency(unittest.TestCase):
    """A USD index at 100, 110, 121 with EURUSD=X at 1.25, 1.10, 1.21 USD per EUR:
    in EUR 100 / 1.25 = 80, 110 / 1.10 = 100, 121 / 1.21 = 100; growth of 10 000 EUR from the
    first day: 10 000, 12 500, 12 500. The first point is the capital itself."""
    I = mk("I", "USD", [("2001-01-02", 100), ("2002-01-02", 110), ("2003-01-02", 121)])
    FX = M.FxTable([D(2001, 1, 2), D(2002, 1, 2), D(2003, 1, 2)], [1.25, 1.10, 1.21])

    def test_in_eur(self):
        b = M.benchmark_in(self.I, self.I.dates, 10000.0, D(2001, 1, 2), self.FX, "EUR")
        self.assertEqual(b[0], 10000.0)
        self.assertAlmostEqual(b[1], 12500.0)
        self.assertAlmostEqual(b[2], 12500.0)

    def test_in_its_own_currency_it_is_the_price_benchmark(self):
        b = M.benchmark_in(self.I, self.I.dates, 10000.0, D(2001, 1, 2), self.FX, "USD")
        for x, y in zip(b, M.benchmark(self.I, self.I.dates, 10000.0, D(2001, 1, 2))):
            self.assertAlmostEqual(x, y)

    def test_a_usd_index_from_2000_in_eur_needs_the_ecb(self):
        """^GSPC's max sample from 2000-01-01 converts through the ECB table before 2003-12. The sample's
        bars are quarterly, so the timeline starts at the inception itself, and its first point is the
        capital."""
        S = M.load_samples()
        gspc = S["indices"][0][2]
        days = [D(2000, 1, 1)] + [d for d in gspc.dates if d > D(2000, 1, 1)]
        b = M.benchmark_in(gspc, days, 10000.0, D(2000, 1, 1), S["fx"], "EUR")
        self.assertEqual(b[0], 10000.0)
        with self.assertRaises(M.NoFxRate):
            M.benchmark_in(gspc, days, 10000.0, D(2000, 1, 1), M.FxTable.from_series(S["series"]["EURUSD=X"]), "EUR")


if __name__ == "__main__":
    unittest.main()
