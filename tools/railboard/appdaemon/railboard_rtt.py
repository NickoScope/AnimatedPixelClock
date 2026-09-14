"""Rail board: Realtime Trains for the LED panel, from AppDaemon.

Polls RTT for the station the panel chose (input_text.railboard_crs, which the
package's "follow the station" automation keeps), exchanges the refresh token
for a short-life access token in memory, and publishes the same retained MQTT
payloads the package's poll automation published:

  nickoscope_matrix/railboard/<crs>/departures | arrivals | status

Why AppDaemon and not a Home Assistant automation: a response variable holding
the access token would be written into the automation's trace, and traces are
kept on disk (.storage/trace.saved_traces). Here both tokens live only in this
process. They are never logged: the log says what happened, never with what.
Publishing goes straight to the broker with paho, as the flight_board app in
this AppDaemon already does, so Home Assistant's recorder does not store a
call_service event every 20 s either.

Install: src/railboard/README.md, "Home Assistant", and apps_railboard.yaml
next to this file. The transform and the client are tools/railboard/rtt_client.py,
which must be copied into the same apps directory.
"""
import json
import threading
import time

import appdaemon.plugins.hass.hassapi as hass
import paho.mqtt.client as mqtt

import rtt_client as rtt


class RailboardRtt(hass.Hass):
    def initialize(self):
        a = self.args
        self._client = rtt.RttClient(a["rtt_token"], kind=a.get("rtt_kind", "refresh"))
        self._base = a.get("topic_base", "nickoscope_matrix/railboard")
        self._entity = a.get("station_entity", "input_text.railboard_crs")
        self._default = a.get("default_crs", "GLD")
        # 20 s at the fastest: the panel's own fetch spends the same daily quota.
        self._interval = max(20, int(a.get("interval", 20)))
        self._busy = threading.Lock()
        self._last_err = None

        self._mq = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="appdaemon-railboard-rtt")
        if a.get("mqtt_user"):
            self._mq.username_pw_set(a["mqtt_user"], a.get("mqtt_pass"))
        self._mq.connect_async(a["mqtt_host"], int(a.get("mqtt_port", 1883)), 30)
        self._mq.loop_start()

        self.listen_state(self._station_changed, self._entity)
        self.run_every(self._poll, "now", self._interval)
        self.log("rail board: polling Realtime Trains every %d s, token kind %s"
                 % (self._interval, a.get("rtt_kind", "refresh")))

    def terminate(self):
        try:
            self._mq.loop_stop()
            self._mq.disconnect()
        except Exception:   # shutting down either way
            pass

    def _station(self):
        s = self.get_state(self._entity)
        return s if isinstance(s, str) and rtt.CRS_RE.fullmatch(s) else self._default

    # Accepts both callback conventions: older AppDaemon passes one kwargs dict
    # positionally (the flight_board app's run_in lambda takes one argument),
    # newer passes keywords.
    def _station_changed(self, entity, attribute, old, new, *args, **kwargs):
        if new != old:
            self.run_in(self._poll, 1)

    def _poll(self, *args, **kwargs):
        if not self._busy.acquire(blocking=False):   # one request at a time
            return
        try:
            crs = self._station()
            now = int(time.time())
            res = self._client.fetch(crs, now)
            if not res["err"]:
                lists = rtt.normalise(res["content"], now, crs)
                if lists is None:
                    res["err"] = "BAD"
                else:
                    for leaf, payload in rtt.board_payloads(crs, lists, now).items():
                        self._publish("%s/%s/%s" % (self._base, crs, leaf), payload)
            self._publish("%s/%s/status" % (self._base, crs), rtt.status_payload(res, now))
            # Say it once when it changes and whenever a request was actually
            # made; a back-off repeats nothing.
            if res["err"] and (res["requests"] or res["err"] != self._last_err):
                self.log("rail board: Realtime Trains %s: %s%s, token kind %s%s"
                         % (crs, res["err"], (" HTTP %d" % res["code"]) if res["code"] else "", res["kind"],
                            (", next request in %d s" % res["backoff"]) if res["backoff"] else ""),
                         level="WARNING")
            elif not res["err"] and self._last_err:
                self.log("rail board: Realtime Trains %s: OK again, token kind %s" % (crs, res["kind"]))
            self._last_err = res["err"] or None
        except Exception as e:   # the type only: a message is not needed and could quote too much
            self.log("rail board: %s while polling" % type(e).__name__, level="WARNING")
        finally:
            self._busy.release()

    def _publish(self, topic, payload):
        self._mq.publish(topic, json.dumps(payload, separators=(",", ":")), qos=0, retain=True)
