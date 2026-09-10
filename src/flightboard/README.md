# Flight board

Everything for this feature lives in this repository.

| | |
|---|---|
| `flightboard.cpp` | the layout constants, written by hand **only** here |
| `tools/fb_render.py` | host render, reads the constants from the source |
| `sim/flightboard-sim.html` | browser simulation, layout block is generated |

## Changing the layout

Edit `flightboard.cpp`, then:

```bash
python3 tools/fb_sim_build.py    # push the constants into the simulation
python3 tools/fb_check.py        # exit 1 if anything is stale
```

`.githooks/pre-commit` runs the check automatically. Enable once per clone:

```bash
git config core.hooksPath .githooks
```

## Why the status words are short

Every pixel the status column takes is a pixel the destination name does not
get. On 30 live Nice rows, `ENROUTE` (29 px) and `DELAYED` (28 px) alone cost
four destinations their name; shortening just those two to `IN AIR` and `DELAY`
took coverage from 23/30 to 27/30. Coverage is flat from 20 px to 28 px, so
`ON TIME`, `LANDED` and `DEPART` keep their spelling.

## Transport

Home Assistant already serves this board over MQTT for another device in the
house, so the panel is a second subscriber to a deployed contract rather than a
new feature.

```
request   nickoscope_watch/flightboard/req            {"apt":"LFMN","dir":"arr"}
response  nickoscope_watch/flightboard/state/<apt>/<dir>   retained
```

Keyed on (airport, direction), not on the client: two devices watching
different airports would otherwise overwrite each other's payload *and* pay
twice for the same AeroAPI fetch, because Home Assistant's throttle only
short-circuits when the currently selected airport matches.

Three things `fb_mqtt.cpp` gets right that are easy to get wrong:

**The buffer.** PubSubClient defaults to 256 bytes and does not truncate an
oversized PUBLISH - it drops it, silently. A live Nice board measured 1060
bytes on the wire. The default would have produced a page that never updated
with nothing in the log to explain it. Set to 2048.

**Resubscribing.** Subscriptions do not survive a reconnect, so the subscribe
runs from the connect path, not only on a selection change.

**Not asking when there is no need.** A retained payload lands within
milliseconds of subscribing - measured, not assumed. A request is published
only if none does within 1.5 s, so turning the knob through six airports costs
one subscribe and at most one fetch. Each fetch is roughly a cent of AeroAPI.

The knob's steps are debounced 1.2 s before any of that happens.

### Credentials

Broker and key live in NVS and are never compiled in. Write them once:

```bash
pio run -e provision -t upload \
  --project-option="build_flags=-DPROV_MQTT_HOST=\\\"192.168.4.35\\\" -DPROV_MQTT_USER=\\\"...\\\" -DPROV_MQTT_PASS=\\\"...\\\""
```

then flash `matrix-waveshare-rgb` over it.

### When there is no data

The page names the reason - `NO BROKER`, `NO WIFI`, `CONNECTING`, `FETCHING`,
`NO DATA` - rather than showing a bare "NO DATA" that sends you looking at Home
Assistant when the panel never reached WiFi.

### Verified

The contract was exercised against the live broker on 2026-09-10: subscribe to
`.../state/LFMN/arr` returned a retained 1060-byte payload immediately, 15
flights, `upd=09:34`. The firmware path itself is **not** hardware-verified.
