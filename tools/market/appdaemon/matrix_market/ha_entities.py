"""Feature `ha_entities`: MQTT discovery of the Home Assistant sensors (docs/18, "HA entities too").

No new state topics: every sensor reads one of the app's retained payloads with a
value_template, so the panel's contract stays the only contract. The sensors
follow the mode the panel shows (portfolio.rebalance) and the window of
display.preset; a config change republishes the discovery set and removes the
entities that left. Availability is the app's `ha` will.

Discovery goes out after the broker answers (qos 1, retained) and again whenever Home Assistant
publishes `online` on <prefix>/status. Entity ids come from `default_entity_id`.

  sensor.matrix_market_portfolio_value        portfolio/<mode>/MAX      .value   (currency)
  ..._portfolio_return                         portfolio/<mode>/<preset> .chg x 100   (%; the TWR)
  ..._portfolio_since_start, _cagr, _mdd, _div, _ter_drag, _cash, _mode, _asof
  ..._portfolio_xirr, _twr_ann, _fx            stats/<mode>/<preset>  (docs/19 §6.1, §6.7)
  ..._vol, _sharpe, _sortino, _calmar, _best_year, _worst_year, _roll1y, _roll3y, _mdd_month_end
  ..._benchmark_1..3                           stats .bench (chg of the window, sym as attribute)
  ..._class_<equity|bond|real_assets|gold|cash>   stats .classes
  ..._holding_<sym>_share, _holding_<sym>_return  holdings/<mode> rows
  ..._exchange_<name>                          tape .x
  ..._status, _status_asof                     status
  binary_sensor.matrix_market_alert_day_move | _alert_drawdown | _alert_out_of_band   (alerts.*, off by default)
"""
import re
from typing import Dict, List, Tuple

from . import __version__
from .features import Feature
from .registry import FEATURES


def object_id(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def _row_template(sym: str, field: str, scale: str = "", digits: int = 2) -> str:
    return ("{%% set r = value_json.rows | selectattr('sym','eq','%s') | list %%}"
            "{{ ((r[0].%s %s) | round(%d)) if (r and r[0].%s is not none) else none }}" % (sym, field, scale, digits, field))


def _num(path: str, scale: str = "", digits: int = 2) -> str:
    return "{{ ((value_json.%s %s) | round(%d)) if value_json.%s is not none else none }}" % (path, scale, digits, path)


def _num_opt(path: str, scale: str = "", digits: int = 2) -> str:
    """As _num, for a key that may be absent."""
    parts = path.split(".")
    guard = " and ".join("value_json.%s is defined" % ".".join(parts[:i + 1]) for i in range(len(parts)))
    return "{{ ((value_json.%s %s) | round(%d)) if (%s and value_json.%s is not none) else none }}" % (path, scale, digits, guard, path)


def sensors(settings, topics, device: str) -> List[Tuple[str, str, dict]]:
    """[(component, object_id, discovery config)], from the settings."""
    mode = settings.mode()
    cur = settings["portfolio.currency"]
    preset = settings["display.preset"]
    pf_max = topics.leaf("portfolio/%s/MAX" % mode)
    pf_win = topics.leaf("portfolio/%s/%s" % (mode, preset))
    stats_win = topics.leaf("stats/%s/%s" % (mode, preset))
    stats_max = topics.leaf("stats/%s/MAX" % mode)
    out: List[Tuple[str, str, dict]] = []

    def add(oid: str, name: str, topic: str, template: str, unit=None, icon=None, state_class=None, attrs=None, component="sensor"):
        cfg = {"name": name, "state_topic": topic, "value_template": template}
        if unit:
            cfg["unit_of_measurement"] = unit
        if icon:
            cfg["icon"] = icon
        if state_class:
            cfg["state_class"] = state_class
        if attrs:
            cfg["json_attributes_topic"] = topic
            cfg["json_attributes_template"] = attrs
        out.append((component, oid, cfg))

    add("portfolio_value", "Portfolio value", pf_max, _num("value"), cur, "mdi:chart-line", "measurement",
        '{{ {"from": value_json["from"], "to": value_json.to, "mode": value_json.mode, "preset": value_json.preset, "measure": value_json.measure} | tojson }}')
    add("portfolio_return", "Portfolio return %s (TWR)" % preset, pf_win, _num("chg", "* 100"), "%", "mdi:percent")
    add("portfolio_since_start", "Portfolio since start", pf_max, _num("sinceStart", "* 100"), "%", "mdi:percent")
    add("portfolio_cagr", "Portfolio CAGR", pf_max, _num("cagr", "* 100"), "%", "mdi:trending-up")
    add("portfolio_mdd", "Portfolio max drawdown %s" % preset, pf_win, _num("mdd", "* 100"), "%", "mdi:trending-down")
    add("portfolio_div", "Portfolio dividends", pf_max, _num("div"), cur, "mdi:cash-multiple")
    add("portfolio_ter_drag", "Portfolio TER drag (est.)", pf_max, _num("terDrag"), cur, "mdi:cash-minus")
    add("portfolio_cash", "Portfolio cash", pf_max, _num("cash", "* 100"), "%", "mdi:cash")
    add("portfolio_mode", "Portfolio mode", pf_max, "{{ value_json.mode | upper }}", None, "mdi:swap-horizontal")
    add("portfolio_asof", "Portfolio as of", pf_max, "{{ value_json.to }}", None, "mdi:calendar")
    # the v2 figures, from the stats topic (docs/19 §6.1, §6.6, §6.7)
    add("portfolio_xirr", "Portfolio XIRR since inception (ann.)", stats_max, _num("xirr_ann", "* 100"), "%", "mdi:percent")
    add("portfolio_twr_ann", "Portfolio return %s (ann.)" % preset, stats_win, _num("ann", "* 100"), "%", "mdi:percent")
    add("portfolio_fx", "Portfolio FX effect %s" % preset, stats_win, _num("fx_effect", "* 100"), "%", "mdi:currency-eur")
    add("portfolio_vol", "Portfolio volatility %s (ann.)" % preset, stats_win, _num("vol", "* 100"), "%", "mdi:sine-wave")
    add("portfolio_sharpe", "Portfolio Sharpe %s" % preset, stats_win, _num("sharpe"), None, "mdi:scale-balance")
    add("portfolio_sortino", "Portfolio Sortino %s" % preset, stats_win, _num("sortino"), None, "mdi:scale-balance")
    add("portfolio_calmar", "Portfolio Calmar %s" % preset, stats_win, _num("calmar"), None, "mdi:scale-balance")
    add("portfolio_mdd_dates", "Portfolio max drawdown %s dates" % preset, stats_win,
        "{{ value_json.mdd.peak ~ ' > ' ~ value_json.mdd.trough ~ ' > ' ~ (value_json.mdd.recovery or 'open') if value_json.mdd.peak else none }}",
        None, "mdi:calendar-range", None, '{{ value_json.mdd | tojson }}')
    add("portfolio_mdd_month_end", "Portfolio max drawdown %s (month ends)" % preset, stats_win, _num("mddME.mdd", "* 100"), "%", "mdi:trending-down")
    add("portfolio_best_year", "Portfolio best year", stats_max, _num_opt("bestY.ret", "* 100"), "%", "mdi:calendar-star",
        None, '{{ {"year": value_json.bestY.y if value_json.bestY else none} | tojson }}')
    add("portfolio_worst_year", "Portfolio worst year", stats_max, _num_opt("worstY.ret", "* 100"), "%", "mdi:calendar-remove",
        None, '{{ {"year": value_json.worstY.y if value_json.worstY else none} | tojson }}')
    add("portfolio_roll1y", "Portfolio rolling 1y", stats_max, _num_opt("roll1y.last", "* 100"), "%", "mdi:chart-timeline-variant",
        None, '{{ value_json.roll1y | tojson if value_json.roll1y else "{}" }}')
    add("portfolio_roll3y", "Portfolio rolling 3y", stats_max, _num_opt("roll3y.last", "* 100"), "%", "mdi:chart-timeline-variant",
        None, '{{ value_json.roll3y | tojson if value_json.roll3y else "{}" }}')
    for i in range(3):
        add("benchmark_%d" % (i + 1), "Benchmark %d return %s" % (i + 1, preset), stats_win,
            "{{ ((value_json.bench[%d].chg * 100) | round(2)) if value_json.bench | count > %d else none }}" % (i, i), "%", "mdi:chart-bell-curve",
            None, '{{ (value_json.bench[%d] | tojson) if value_json.bench | count > %d else "{}" }}' % (i, i))
    for cls in ("equity", "bond", "real_assets", "gold", "cash"):
        add("class_%s" % cls, "Allocation %s" % cls.replace("_", " "), stats_max, _num_opt("classes.%s" % cls, "", 1), "%", "mdi:chart-pie")
    hold = topics.leaf("holdings/%s" % mode)
    for p in settings.positions():
        oid = object_id(p["sym"])
        add("holding_%s_share" % oid, "%s share" % p["sym"], hold, _row_template(p["sym"], "now", "", 1), "%", "mdi:chart-pie")
        add("holding_%s_return" % oid, "%s return since entry" % p["sym"], hold, _row_template(p["sym"], "ret", "* 100", 1), "%", "mdi:percent")
    add("cash_share", "Cash share", hold, _num("cash.now", "", 1), "%", "mdi:cash")
    tape = topics.leaf("tape")
    for name in settings.get("tape.exchanges") or []:
        add("exchange_%s" % object_id(name), "%s" % name, tape,
            "{%% set r = value_json.x | selectattr('n','eq','%s') | list %%}{{ r[0].s if r else none }}" % name, None, "mdi:bank")
    status = topics.leaf("status")
    add("status", "Market data", status, "{{ value_json.state }}", None, "mdi:database-check")
    add("status_asof", "Market data as of", status, "{{ value_json.asof }}", None, "mdi:calendar-clock")
    # alerts: binary sensors, only when switched on (docs/18: HA only, no notifications)
    live = topics.leaf("live")
    day_move = float(settings.get("alert.day_move_pct") or 0)
    if day_move > 0:
        add("alert_day_move", "Alert: a symbol moved over %g %% today" % day_move, live,
            "{%% set m = value_json.q.values() | selectattr('day','defined') | map(attribute='day') | map('abs') | list %%}"
            "{{ 'ON' if m and (m | max) > %s else 'OFF' }}" % (day_move / 100.0), None, "mdi:alert", component="binary_sensor")
    dd = float(settings.get("alert.drawdown_pct") or 0)
    if dd > 0:
        add("alert_drawdown", "Alert: portfolio over %g %% below its peak" % dd, stats_max,
            "{{ 'ON' if value_json.ddNow is not none and value_json.ddNow < %s else 'OFF' }}" % (-dd / 100.0), None, "mdi:alert", component="binary_sensor")
    if settings.get("alert.out_of_band"):
        add("alert_out_of_band", "Alert: a holding is outside its band", hold,
            "{{ 'ON' if (value_json.rows | selectattr('out','defined') | list | count) > 0 else 'OFF' }}", None, "mdi:alert", component="binary_sensor")
    dev = {"identifiers": ["matrix_market_%s" % device], "name": "Matrix Market", "manufacturer": "NickoScope",
           "model": "matrix_market (AppDaemon)", "sw_version": __version__}
    avail = {"topic": topics.ha, "payload_available": "online", "payload_not_available": "offline"}
    for component, oid, cfg in out:
        cfg["unique_id"] = "matrix_market_%s_%s" % (device, oid)
        # HA 2026.4 removed `object_id` from MQTT; discovery now ignores it. `default_entity_id` is the whole
        # entity id, domain included ("such as update.foobar"), applied when the entity is first added
        # (home-assistant.io 2026-04-01 release notes and the MQTT platform docs, via context7, 2026-09-15).
        cfg["default_entity_id"] = "%s.matrix_market_%s" % (component, oid)
        cfg["device"] = dev
        cfg["availability"] = [avail]
        cfg["has_entity_name"] = True
    return out


@FEATURES.register("ha_entities")
class HaEntitiesFeature(Feature):
    name = "ha_entities"
    order = 50

    def __init__(self, ctx, device: str = ""):
        super().__init__(ctx)
        self.device = device or ctx.data.get("device", "000000")
        self.published: Dict[str, dict] = {}          # discovery topic -> config, as last sent

    def discovery_topic(self, component: str, oid: str) -> str:
        return "%s/%s/matrix_market_%s/%s/config" % (self.settings["ha.prefix"], component, self.device, oid)

    @property
    def status_topic(self) -> str:
        """Home Assistant's birth and will topic: `online` after HA (re)starts, when discovery must be sent again."""
        return "%s/status" % self.settings["ha.prefix"]

    def publish_all(self, force: bool = False) -> None:
        """Every discovery config at QoS 1, retained (paho queues QoS 1 while disconnected and drops QoS 0; audit M3).
        `force` sends every config again whatever was sent before: after a (re)connect, or when HA says online."""
        if not self.settings["ha.discovery"]:
            return
        client = self.ctx.publisher.client
        new = {self.discovery_topic(c, oid): cfg for c, oid, cfg in sensors(self.settings, self.ctx.publisher.topics, self.device)}
        for topic in sorted(set(self.published) - set(new)):
            client.publish(topic, b"", qos=1, retain=True)     # an entity that left
        from .publisher import encode
        for topic, cfg in new.items():
            if force or self.published.get(topic) != cfg:
                client.publish(topic, encode(cfg), qos=1, retain=True)
        self.published = new

    def start(self) -> None:
        pass          # nothing before the broker has answered: discovery goes out from on_connect (audit M3)

    def on_connect(self) -> None:
        self.publish_all(force=True)

    def on_ha_status(self, payload: bytes) -> None:
        if payload.strip().lower() == b"online":
            self.log("Home Assistant is online: discovery sent again")
            self.publish_all(force=True)

    def on_config(self, settings) -> None:
        self.publish_all()

    def stop(self) -> None:
        pass          # the retained discovery stays: Home Assistant keeps the entities across a restart of the app
