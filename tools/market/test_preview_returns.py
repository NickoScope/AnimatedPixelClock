#!/usr/bin/env python3
"""Tests of preview_returns.py, the independent oracle for TWR, ANN, XIRR and the drawdown dates.
Every expected number is worked by hand in the docstrings.

  python3 -m unittest tools/market/test_preview_returns.py -v
"""
import datetime as dt
import math
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import market_ref as M  # noqa: E402
import preview_returns as R  # noqa: E402

D = dt.date


def daily_product(values, flows):
    """Portfolio Performance's formula literally, one factor a day, for the cross-checks."""
    g = 1.0
    for i in range(1, len(values)):
        g *= (values[i] + max(-flows[i], 0.0)) / (values[i - 1] + max(flows[i], 0.0))
    return g


class TwrByHand(unittest.TestCase):
    """Five closes, one flow in and one out, Portfolio Performance's 1 + r = (MVE + CFout) / (MVB + CFin):

      day  flow    close    1 + r
      0      -    10 000    the opening value
      1      -    11 000    11 000 / 10 000                 = 1.1
      2   +1 000  13 200    13 200 / (11 000 + 1 000)       = 1.1      in at the start of the day
      3      -    11 880    11 880 / 13 200                 = 0.9
      4   -2 000  10 000    (10 000 + 2 000) / 11 880       = 12 000 / 11 880   out at the end, before the valuation

      linked: 1.1 x 1.1 x 0.9 x 12 000 / 11 880 = 1.089 x 12 000 / 11 880 = 13 068 / 11 880 = 1.1, TWR +10 %,
      while last / first - 1 = 10 000 / 10 000 - 1 = 0.
    """
    V = [10000.0, 11000.0, 13200.0, 11880.0, 10000.0]
    F = [0.0, 0.0, 1000.0, 0.0, -2000.0]

    def test_linked(self):
        self.assertAlmostEqual(R.twr_growth(self.V, self.F), 1.1, places=12)

    def test_the_same_as_day_by_day(self):
        self.assertAlmostEqual(R.twr_growth(self.V, self.F), daily_product(self.V, self.F), places=12)

    def test_no_flows_is_last_over_first_bit_for_bit(self):
        """No flow: the factor is v[-1] / v[0] itself, so TWR is the same float as market_ref's chg."""
        v = [10000.0, 12345.678, 9876.5, 13000.125]
        self.assertEqual(R.twr_growth(v, [0.0] * 4) - 1.0, v[-1] / v[0] - 1.0)

    def test_annualised_only_from_a_year(self):
        """1.21 over exactly two calendar years, 2021-01-04..2023-01-04 = 730 days = 730 / 365.25 years:
        1.21^(365.25 / 730) - 1, a hair above 10 %. 364 days is under a year: None."""
        self.assertAlmostEqual(R.annualised(1.21, D(2021, 1, 4), D(2023, 1, 4)), 1.21 ** (365.25 / 730) - 1, places=15)
        self.assertIsNone(R.annualised(1.21, D(2021, 1, 4), D(2022, 1, 3)))
        self.assertIsNotNone(R.annualised(1.21, D(2021, 1, 4), D(2022, 1, 4)))


class XirrByHand(unittest.TestCase):
    """Microsoft's XIRR: sum P_i / (1 + r)^((d_i - d_1) / 365) = 0.

    One year, 2021-01-04 to 2022-01-04 (365 days): -1 000, +1 100.  1 100 / (1 + r) = 1 000, r = 10 %.
    Two whole years, 2021-01-04, 2022-01-04, 2023-01-04 (0, 365, 730 days): -1 000, -1 000, +2 310.
      At 10 %: -1 000 - 1 000 / 1.1 + 2 310 / 1.21 = -1 000 - 909.0909 + 1 909.0909 = 0, r = 10 %.
    """

    def test_one_year(self):
        self.assertAlmostEqual(R.xirr([D(2021, 1, 4), D(2022, 1, 4)], [-1000.0, 1100.0]), 0.10, places=12)

    def test_two_years(self):
        r = R.xirr([D(2021, 1, 4), D(2022, 1, 4), D(2023, 1, 4)], [-1000.0, -1000.0, 2310.0])
        self.assertAlmostEqual(r, 0.10, places=12)

    def test_order_of_the_payments_does_not_matter(self):
        r = R.xirr([D(2023, 1, 4), D(2021, 1, 4), D(2022, 1, 4)], [2310.0, -1000.0, -1000.0])
        self.assertAlmostEqual(r, 0.10, places=12)

    def test_refuses_without_both_signs(self):
        with self.assertRaises(ValueError):
            R.xirr([D(2021, 1, 4), D(2022, 1, 4)], [-1000.0, -1.0])


class LedgerWithContributions(unittest.TestCase):
    """market_ref's ledger: one fund, 10 000 EUR from 2021-01-04, 1 000 on each anniversary, HOLD.

      bar         close  what happens                                   V
      2021-01-04   100   buy 100 sh                                     10 000
      2021-12-31   110   year-end, no cash                              11 000
      2022-01-04   110   +1 000 lands in the cash                       12 000
      2022-12-30   121   year-end: the 1 000 buys 1 000 / 121 sh        12 100 + 1 000 = 13 100
      2023-01-04   121   +1 000 lands in the cash                       14 100

    TWR, day by day: 11 000 / 10 000 = 1.1; 12 000 / (11 000 + 1 000) = 1; 13 100 / 12 000; 14 100 / (13 100 + 1 000) = 1
      linked 1.1 x 13 100 / 12 000 = 14 410 / 12 000 = 1.200 833..: TWR +20.083 %.
      The payload's chg, 14 100 / 10 000 - 1 = +41 %, counts the 2 000 put in as a return.
    ANN over 730 days = 730 / 365.25 years: (14 410 / 12 000)^(365.25 / 730) - 1.
    XIRR: -10 000 at day 0, -1 000 at day 365, -1 000 and +14 100 at day 730 (net +13 100). Times (1 + r)^2,
      with x = 1 + r: -10 000 x^2 - 1 000 x + 13 100 = 0, so
      x = (-1 000 + sqrt(1 000^2 + 4 x 10 000 x 13 100)) / (2 x 10 000) = (-1 000 + sqrt(525 000 000)) / 20 000
        = (-1 000 + 22 912.878) / 20 000 = 1.095 644: XIRR +9.564 %.
    """
    A = M.Series("A", "EUR", [D(2021, 1, 4), D(2021, 12, 31), D(2022, 1, 4), D(2022, 12, 30), D(2023, 1, 4)],
                 [100.0, 110.0, 110.0, 121.0, 121.0])
    C = M.Config(10000.0, "EUR", D(2021, 1, 4), [M.Position("A", 100.0)], contrib_amount=1000.0, contrib_every="year")
    FX = M.FxTable([D(2000, 1, 1)], [1.0])

    def ledger(self):
        return M.simulate(self.C, {"A": self.A}, self.FX, self.A.dates, "hold")

    def test_the_ledger_is_as_worked(self):
        L = self.ledger()
        for got, want in zip(L.v, [10000, 11000, 12000, 13100, 14100]):
            self.assertAlmostEqual(got, want, places=9)
        self.assertEqual(R.ledger_flows(L), [0.0, 0.0, 1000.0, 0.0, 1000.0])

    def test_twr_ann_xirr(self):
        L = self.ledger()
        w = R.window_figures(self.C, L, "MAX")
        self.assertAlmostEqual(w["twr"], 14410 / 12000 - 1, places=12)
        self.assertAlmostEqual(w["ann"], (14410 / 12000) ** (365.25 / 730) - 1, places=12)
        self.assertAlmostEqual(w["xirr"], (-1000 + math.sqrt(525e6)) / 20000 - 1, places=9)
        self.assertAlmostEqual(w["flows"], 2000.0)
        p = M.portfolio_payload(self.C, L, M.simulate(self.C, {"A": self.A}, self.FX, self.A.dates, "hold", False), None, "MAX")
        self.assertAlmostEqual(p["chg"], 0.41, places=9)


class NoFlowsMatchTheLedger(unittest.TestCase):
    """The preview portfolio on the saved samples (VOO alone, no contributions): the TWR is the payload's
    chg bit for bit on every window, ANN is its cagr wherever cagr is reported (from 3 years), the
    drawdown's depth is its mdd, and there is no XIRR. With 1 000 a year the stretch-wise TWR equals
    the literal day-by-day product."""
    @classmethod
    def setUpClass(cls):
        cls.P = M.run_preview_portfolio()

    def test_every_preset_both_modes(self):
        P = self.P
        for mode in ("hold", "rebal"):
            L, Ln = P["runs"][mode], P["runs"][mode + "_nodiv"]
            for preset in M.PRESETS:
                p = M.portfolio_payload(P["cfg"], L, Ln, P["bench"], preset)
                w = R.window_figures(P["cfg"], L, preset)
                self.assertEqual(w["twr"], p["chg"], (mode, preset))
                if p["cagr"] is not None:
                    self.assertEqual(w["ann"], p["cagr"], (mode, preset))
                self.assertEqual(w["drawdown"]["mdd"], p["mdd"], (mode, preset))
                self.assertIsNone(w["xirr"])
        self.assertIsNone(R.window_figures(P["cfg"], P["runs"]["hold"], "YTD")["ann"])

    def test_contributions_stretches_equal_daily_product(self):
        P, S = self.P, self.P["samples"]
        c = M.Config(10000.0, "EUR", P["cfg"].inception, P["cfg"].positions, 1000.0, "year")
        L = M.simulate(c, S["series"], S["fx"], P["days"], "hold", True, M.calendar_years)
        flows = R.ledger_flows(L)
        self.assertEqual(sum(flows), L.contrib[-1])
        g = R.twr_growth(L.v, flows)
        self.assertAlmostEqual(g / daily_product(L.v, flows), 1.0, places=12)


class DrawdownDates(unittest.TestCase):
    """Closes 100, 120, 120, 90, 110, 125, 100, 130 on the month starts 2020-01 .. 2020-08:
      the running peak 120 is held on 02-01 and 03-01; 90 on 04-01 is 90 / 120 - 1 = -25 %;
      125 on 06-01 is back above 120, the recovery; 100 on 07-01 is 100 / 125 - 1 = -20 %, shallower.
      MDD -25 %, peak 2020-03-01 (the last day at 120), trough 2020-04-01, recovery 2020-06-01.
    The first five closes alone end at 110, under 120: no recovery."""
    DAYS = [D(2020, m, 1) for m in range(1, 9)]
    V = [100.0, 120.0, 120.0, 90.0, 110.0, 125.0, 100.0, 130.0]

    def test_recovered(self):
        dd = R.drawdown(self.DAYS, self.V)
        self.assertEqual((dd["peak"], dd["trough"], dd["recovery"]), (D(2020, 3, 1), D(2020, 4, 1), D(2020, 6, 1)))
        self.assertAlmostEqual(dd["mdd"], -0.25)
        self.assertEqual(dd["mdd"], M.max_drawdown(self.V))

    def test_not_recovered(self):
        dd = R.drawdown(self.DAYS[:5], self.V[:5])
        self.assertEqual((dd["peak"], dd["trough"], dd["recovery"]), (D(2020, 3, 1), D(2020, 4, 1), None))

    def test_no_fall(self):
        self.assertEqual(R.drawdown(self.DAYS[:3], [1.0, 2.0, 3.0]), {"mdd": 0.0, "peak": None, "trough": None, "recovery": None})


if __name__ == "__main__":
    unittest.main()
