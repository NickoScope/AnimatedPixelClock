// SyncEngine.swift - Sync with panel: the app's twin and one physical panel kept as one device.
//
// The owner, 2026-09-30 09:15: while the switch is on, the screen (page, effect, clock style,
// brightness, on/off), the settings, the Lua effects (a new one appears, a removed one goes) and the
// firmware are mirrored both ways. Firmware goes to the twin by itself and to the physical panel only
// after a person says yes here ("Update the panel too?"). What sync brings is not sent back (no echo);
// when both sides change the same thing, the later change wins. The owner's overrides on the twin -
// climateHa off (2026-09-29) and fbAskHa off (2026-09-30 22:40), tools/twin/sync.py OVERRIDES - hold always
// and never travel to the panel;
// secrets - the weather API key, and the AeroAPI, RTT and AIS keys of the portal's Keys page - identity
// and state never travel at all. Other owners work in their own forks: firmwareRepo names where releases
// and the gallery come from.
// The owner, 2026-09-30 19:35: the twin keeps no paid screen off by force. Like the panel, it has the Keys
// page (data-page="pkeys", /api/keys: a key is written there and never given out - its GET says only
// whether one is stored, web_panel.cpp:1000-1123), and its flight and rail boards work from the keys
// entered there. So the flights and trains pages, their place in the walk and the flight board's airport
// are mirrored like everything else; the keys never are, in either direction - each device has its own.
// Without a key the board makes no paid call of its own: the AeroAPI fetch stops before anything is counted
// or sent (aero_direct.cpp:725, NO KEY), the RTT one too (rtt_direct.cpp:737, NO TOKEN). Nor does it have one
// made for it: a board without a key and with an MQTT broker in NVS (never set by sync) asks Home Assistant
// for a built-in airport's board (fb_mqtt.cpp:103-136; once per airport and half in 15 min, 12 an hour at
// most, :24-45), and HA fetches it with the owner's AeroAPI key. The owner, 2026-09-30 22:40: a twin without
// a key of its own must not ask. Since firmware 2.7.13 fbAskHa off stops the asking (the retained boards
// still come, free), and sync keeps it off on the twin, an override like climateHa (enforceOverrides); fbAskHa
// is compared on neither side and carried neither way. A twin whose firmware has no fbAskHa yet, with a broker
// set and no AeroAPI key of its own, has its board held on the custom airport ZZZZ "NO REQUESTS", which is never
// asked for (fb_mqtt.cpp mayAsk), and the panel's airport is not carried to it - nor its ZZZZ to the panel -
// until it has a key or fbAskHa; then it takes the panel's airport and ZZZZ goes (holdsNoAsk, Settings.residue).
// The owner, 2026-09-30 19:13: "if the panel runs the carousel, the twin runs it too, with the same
// screens, in step - and the other way round". How: "One carousel for both" below.
// The owner, 2026-09-30, after two reviews: switched on, sync is full, both ways; switched off, all
// stays as it is - nothing is rolled back, and nothing more is written, not even what was half done.
// The settings that describe the panel's own hardware go from the panel to the twin only (Settings.hardware).
// The owner, 2026-09-30 11:19, "SYNC with two confirmations": switching sync on takes two answers - "Enable
// sync with the panel <name, address, MAC>? From now on changes go both ways: settings, effects, screen"
// [Enable / Cancel], then the alignment with the list of differences [From the panel to the twin / From the
// twin to the panel / Cancel] - and until both are given, sync does nothing: it reads who the two devices
// are and what differs, and writes nothing. Flashing the panel takes two as well: "Update the panel too?"
// and "Really flash the physical panel <name> with <version>?". Cancel, or Not now, is the default button
// (Return) in each of these windows, and the buttons that write have no key.
// The consent is remembered (the owner's item 15, 2026-10-01; app 1.4): once both windows are answered, it is kept
// in sync-state.json with the pair's state, as an HMAC of the pair (the two MACs) under the sync state's key in the
// Keychain - so it cannot be written in for another pair - for as long as the switch stays on. The app quitting
// and starting again with the switch on asks nothing: sync resumes by itself, the panel's changes made meanwhile
// go to the twin; only if the twin changed meanwhile is the direction asked, as ever (resume, quietResume). The
// switch turned off - the window, the menu, `defaults write` - forgets it: switching on asks both questions again.
// So does a new pair, or a start with the switch off. The first start of 1.4 over app 1.3's state: 1.3 kept no
// consent, so with the switch on and an aligned state of this very pair in the file (sync-state.json v2), the
// consent counts as given (keptConsent) - once: the file is made 1.4's (v3) there and then, with the consent in it or
// without, and a switch-off makes a v2 file 1.4's without one too (consentDropped). Where the Keychain gives no key,
// nothing is kept and each start asks both.
//
// Both devices are polled over HTTP, one request at a time: the firmware's WebServer serves a request inside
// loop() and the panel stands still meanwhile (web_panel.cpp:1066-1068), and a burst of parallel requests once
// drained the radio's memory (web_heap_backoff.h:4-12). Every request here comes from one worker thread, and
// requests to the panel are paced. Since firmware 2.7.13 a device also pushes what changes, as UDP datagrams
// ("Instant events" below): what a person does is carried at once, and the polling stays as the net under it.
// A 503 is asked again only when it carries Retry-After: that is the firmware standing aside before doing anything (webRefuseBig,
// web.cpp:1421-1456). The Panel group's 503 "out of memory" comes after the work was done, with no
// Retry-After (failOom, web_panel.cpp:138-152; sendDocFromPsram, web.cpp:99-104): a POST that got it is
// not sent again - the round fails, and the next one reads the target again and decides from that.
//
// What is read, and how often (the periods are our choice, not measured; the portal itself polls
// /api/panel every 2 s and /api/info every 5 s while it is open, web_panel_js.h:86, web_pages.h:2376):
//   screen    every 3 s   GET /api/panel (web_panel.cpp:309-393: now.key/name/style/entered, the carousel -
//                         enabled, idleS, slotS, allStyles, running, holdS, pageS, nextS - pages[]) and GET
//                         /api/status (web.cpp:554-580: brightness %, forcedOff, uptime). An uptime smaller
//                         than the last one read - by any round, from /api/status or /api/info, one clock
//                         (millis() / 1000, web.cpp:495, 632) - is a restart: the round stops, and the restart
//                         is dealt with first; its reset screen is nobody's change (restarts, rebooted)
//   step      the panel's GET /api/panel alone, right after its carousel's next step is due (nextS), while
//                         the twin follows it; it stands for the next screen round ("One carousel for both")
//   who       every 15 s  GET /api/info of both: the MAC must still be the pair's; also at once after a
//                         device answered again and when its uptime jumped (an address changes hands)
//   effects   when the Lua names or walk switches in /api/panel change, and every 60 s: GET /api/lua
//                         (web_panel.cpp:968-1062; slow, about 0.5 s on the twin) - nothing more
//   hashes    where the firmware gives out the scripts (GET /api/lua/source), each one's SHA-256, only to see an
//                         edit that keeps its length (app 1.4.1; the owner, 2026-10-01): one script of a side at a
//                         time, never two in a row - hashGap (8 s) between two reads of a side at least, each script
//                         again about once in hashCircle (5.5 min: 35 scripts, one each 9.4 s); a script new to the
//                         list or changed in size first, and it is not compared until its SHA-256 is read
//                         (Effects.unknown). Both sides' are kept in sync-state.json: a start that asks nothing
//                         reads none at once (the twin runs only while the app does: its scripts cannot change
//                         meanwhile); a question, which lists what differs, reads all of both (settle). Each read holds the panel's loop() (35 in a row every 5 min held it up
//                         to 3.8 s each; app 1.4, the owner's pair, 2026-10-01 05:44-06:06). A script is asked for by its
//                         file's name, exactly as GET /api/lua lists it (uploaded.scripts[].name). One a
//                         side lists but answers 404 for (NoSuchScript: the panel's LA_GIOCONDA, 30.09 21:23 -
//                         22:58, a 404 in every round, which the round then failed on) is said once and not
//                         asked for again until its size changes (noSource); it is compared by size meanwhile
//   settings  every syncSettingsEveryS (60 s): /api/info, /api/portal, /api/export, /api/knob,
//             /api/worldclock, /api/railboard, /api/flightboard, /api/media, /api/ir; /api/market (30 KB)
//             every fifth time. /api/info also carries the firmware: version, build, firmwareBytes,
//             ota.state (web.cpp:431-551), read every 15 s while a new image is still "pending" or
//             "new" (boot_health.cpp:36-45), five minutes at most.
//   load      /api/info's webRefused and allocFails (web.cpp:477-485): when either grew since the last
//             round the panel is under strain, and for the next 10 min (our choice) the screen is read
//             every 5 s (the portal's own cadence, net_turns.h:32), the panel's carousel step is not looked
//             for in between (the twin follows it a screen round later), and the settings round leaves 1.6 s
//             between the panel's requests - more than NET_TURN_QUIET_MS, so the panel's own fetches are
//             not held back by it (net_turns.h:47).
//   failure   a device that does not answer, or a round that failed: the next screen round in 5 s, and
//             nothing before it - no step read, no effects or settings round (backOff).
//   long      the rounds that take a while - effects, settings, firmware, a script's SHA-256 - serve the instant events
//             between any two of their requests (Device.before -> LongRounds.between; app 1.4.1), and a read of theirs
//             under way when a person's change (or the carousel's step) comes from the side it reads gives way: it is
//             cut off, the change is carried to the other side, and it is sent again (Device.yields, once at most; never
//             a write, never the firmware image). A script's source read on its schedule gives way to a change from
//             either side, and is made again 3 s later at the soonest (Yielded). A person's change waits for no round
//             (1.4: 1.6-3.8 s in an effects round, 15 s in the 35 reads; GET /api/lua alone is 0.7 s on the panel,
//             1.1 s on the twin).
//   events    (2.7.13) POST /api/sync/listen to each side every 25 s - a write that changes nothing on the screen;
//             with the panel's events coming, its carousel's step is not read but told (followStep); GET /api/panel
//             asks ?input=N (the gestures after N) in the same read; inside a page: /api/railboard of both before a
//             rail board gesture is carried, the target's /api/panel while an effect it is to be clicked in has not
//             opened (0.4 s apart on the panel, openWait at most, once a run of the effect; never the panel under strain),
//             and - only with a firmware before 2.7.13 on either side, while both show one effect - /api/knob in each
//             screen round.
//
// Change, echo, who wins. Each side has a base: what it held after the last round, with what this
// engine wrote to it included - after every write the target is read again and that reading becomes
// its base, so a change that arrived by sync is never taken for the person's and sent back; and
// nothing is written that the target already holds. A side's base moves on only when its change was
// written; a write that failed leaves it where it was, so the change is seen and tried again, and a
// failure on one side does not keep the other side's changes from being written. A change is a field
// whose value differs from its side's base. When both sides changed one field in one round, the page
// goes to the side whose carousel.pageS is smaller - the firmware's only clock of changes, reset by
// every show, knob turn and remote press (carousel.cpp:13-17); for everything else there is no
// timestamp on the device, so the order is known only to within a poll period - 3 s for the screen,
// 60 s for effects, syncSettingsEveryS for settings - and a tie goes to the panel ("conflict: the
// panel's taken - <key>" in the log). A page or style change made by a carousel (carousel.running,
// carousel.cpp:25-27) is never a person's change (Screen.walked); how the twin shows the panel's walk is
// "One carousel for both" below. A screen write whose answer was lost may still land; the side showing it
// later is sync's own write, not a person's change (wrote). The bases of
// settings, effects and firmware are kept in <dataDir>/sync-state.json - the settings as HMAC-SHA-256
// digests whose key is in the login Keychain, not in the file, so no value can be guessed from the
// file - and a restart of the app resumes where it stopped: what the panel changed meanwhile comes to
// the twin; what the twin changed meanwhile is shown first, and the person says whether it goes to the
// panel, or the twin takes the panel's state back, or sync stays off - that is the second window then. The
// first time a pair is synced, the person picks the direction. A question is answered for what it showed:
// after the answer both sides are read again, and if anything it listed changed meanwhile, the question
// comes again. The effects are part of what a question shows: while a side's cannot be known now (its
// /api/lua/source probe got no answer), the question waits for them, a minute at most (our choice); then
// the direction is asked, and effects that could not be read are aligned in the chosen direction once
// both sides' can - with the MASS_DELETES threshold, past which the direction is asked again. Effects
// that have never been aligned are never merged by themselves: the direction is asked.
//
// One carousel for both. What the carousel is - on or off, idleS, slotS, allStyles (/api/panel
// "carousel", NVS carOn/carIdle/carSlot/carAll, panel.cpp:358-361) - and which pages it visits (pages[].on,
// NVS "pages"; each effect's walk switch, NVS "luaOff") are settings like the others, but read in every
// screen round and carried at once, both ways (Settings.walkKey): switched on or off on either side - the
// portal, the remote's carousel button (ir_actions.cpp:137-141), the API - it is on or off on both a round
// later. While it is on, the panel leads and the twin follows: each step of the panel's carousel - a page,
// and with "walk every clock style" a style (main.cpp:1173-1180) - is written to the twin, sync's own write
// (wrote), never taken for a person's change there and never sent back. Each write holds the twin's own
// carousel for the idle time (panelShowPage -> carouselNote, main.cpp:883-889), so the twin does not walk
// by itself; before a hold of the twin's would end ahead of the panel's it is held again (holdTwin), and
// when the carousel is switched on at the twin, the twin waits for the panel's first step. The panel is
// never the follower: each clock style written to it would be saved to its flash 2.5 s later (applyStyle ->
// clockStyleTick -> saveClockStyle, clock_style.cpp:43-50, 136-142), where its own carousel only shows a
// style (showStyle, :52-62, not saved) - so nothing of the twin's walk is ever written to it (Screen.walked):
// with the carousel on the panel leads, with it off there is no walk to follow. To follow within about a
// second rather than a screen round later: every read of the panel's /api/panel gives the seconds to its
// next step (nextS = max(secs - pageS, holdS), whole seconds, web_panel.cpp:345-365); the window of the step
// is narrowed read by read (stepWindow), the panel's /api/panel alone is read just after the step is due
// (stepRead) and the step written to the twin at once; that read stands for the next screen round, so the
// panel is read about as often as before, and the rounds that take a while (effects, settings) wait while a
// step is near. A person's page, style, brightness or on/off on either side is carried as before: it holds
// that side's carousel, the write that carries it holds the other one, both wait the idle time and walk on
// together, the panel first. While a person is at the twin - its carousel's hold began after sync last wrote
// to it, or a page is entered with its knob - the panel's steps are not written over them; as that hold is
// about to end the twin takes the panel's screen again. The twin restarted mid-walk: it takes the panel's
// screen, and nothing of its reset screen is written to the panel. A setting sync wrote to it just before -
// its old boot did not answer lostWithin after the write (a module saves to NVS 2.5 s after a change,
// panel.cpp panelTick) - and that it shows again as it was before the write, is a write it lost: the panel's
// value goes back to it (twinLost, settingsChanges). Whatever else differs on it is its own change, carried as
// ever - a person's choice saved before the restart. Sync switched off: nothing is rolled back, and each
// carousel walks by itself once the twin's hold ends. The panel leads: a write to it that moves its page (a
// renumbered effect list) is not undone - the twin follows its page (putBack). A page is matched by its key and
// the name its effect's banner shows, never by its number: an effect uploaded on the twin by a person renumbers
// its list, and the index it shows then names another effect - that move is nobody's choice, is not carried to
// the panel, and the twin shows the panel's page again (renumbered, shifted).
//
// Instant events (firmware 2.7.13, src/sync/sync_events.cpp; the owner's items 5, 6 and 12, 2026-09-30). Once both
// confirmations are given, sync asks each side for its events - POST /api/sync/listen {"port"} every listenEvery,
// the firmware keeps a listener 60 s and two at most (409 when both places are taken; 404 on a firmware before
// 2.7.13) - and ends the subscription ({"stop":true}) when it is switched off and when the app quits. One UDP socket
// of the app takes the datagrams of both: {"seq","t":"screen","page","key","name","style","entered","off","bright",
// "fx":{"id","run","clicks"},"by"} on every change of the screen (at most one a 100 ms), {"seq","t":"input","kind":
// "press"|"cw"|"ccw","by","page","entered"} on every gesture. "by" says who: a person (knob, ir, http - a request
// without X-Twin-Sync: the portal, Home Assistant), sync (its own write coming back: nothing is done), carousel,
// schedule (the night's off: not compared, forcedOff is), auto (nothing noted). A person's page, style or brightness
// is carried at once, without reading that side - the datagram is its screen - and only the other side is written
// and read back from the answer (carryNow); within crossWindow of sync's own write to that side, or when the other
// side changed the same thing, or for on/off (the datagram gives it with the night's), the screen round runs at once
// instead and judges as ever. The leading panel's carousel step is given to the twin as it comes (followStep: no step
// read of the panel). "auto" runs the screen round at once. A gap in seq is a lost datagram: both sides are read at
// once, with ?input=N - the firmware's ring of 8 gestures - so a lost gesture is taken from there; the polling every
// 3 s goes on whatever comes, and a side with no events (404, 409, a datagram that never arrives) is polled as before.
// Where a device sends from: on the network, its own address, port 4210. Behind the engine's NAT (a twin without the
// home network, a test's "panel" on 127.0.0.1) the device sees this Mac as 10.0.2.2, and a datagram it sends there
// reaches the Mac only as the answer on its forwarded UDP port: the engine sends a guest's datagram from 4210 to the
// gateway's port FWD of a rule --hostfwd udp:FWD-4210 to the host peer that sent to FWD last (esp-soc/src/nat.rs
// udp_out) - so sync sends an empty datagram there from its socket first (the firmware takes nothing from it:
// network.cpp handleUDP) and asks for the datagrams on FWD (eventRoute; main.swift gives the twin's udpPort, the
// test switch syncPanelUdpForTesting the test panel's). Where the twin's datagrams do not come, the twin's page in
// the app tells of each gesture a person makes there (main.swift, "twinInput"), and its console of each effect that
// opens ("[luafx] open"): a moment later the twin alone is read and what changed there carried, the panel's last
// reading standing for its own (twinRound; takeWake: at most one each 0.3 s, none while the panel is under strain).
//
// Inside a screen (the owner, 2026-09-30 20:53: a press of the knob in FLOW changed the scene on its own side only;
// the design, workflow sync-inside-screen-research, tasks/w4kseii7h). Carried only while both sides show the same page:
//   an effect  (key lua) its clicks (px.button: the knob's click, the remote's OK, POST /api/lua {"click":true}). With
//             2.7.13 on both, now.fx {id, open, run, clicks} - clicks since the effect was chosen - is compared
//             (reconcileClicks), and only where it is the page's own effect (fx.id is the page's: Screen.fxHere - right
//             after a page change, until the device's next pass of loop(), it is still the effect before). Each side has a
//             base, the clicks taken into account (sync's own, those carried, those there before sync saw the effect -
//             and, of a run on a page sync or the carousel put there, those it had when first seen); clicks above it are
//             a person's, and the other side is given as many by POST /api/lua {"click":true} (X-Twin-Sync: 1; it holds
//             that side's carousel as the knob's click would, and clicks are never merged, unlike /api/ir/do), once its
//             effect is open, keeping the person's spacing (a double press stays one). Nothing else is ever clicked on
//             the panel: its effect reopened (an upload, a removal) is not brought up to the twin's count; the twin's,
//             opened anew alone on the page sync put there (a restart, back from a person's page), is brought up to the
//             panel's. A side whose effect does not open within openWait (a LUA ERROR) is read for it no longer: the
//             clicks for that run are given up, said once, until it opens or reopens; the panel under strain is not read
//             for it at all (its screen round tells). A click whose answer was lost makes that side's next reading its
//             base. So nothing is echoed, and a lost datagram is made up from the counts within a round. A firmware
//             without now.fx on either side: the knob's clicks (/api/knob stats click + long, read in each screen round
//             only then) grown since the last read on that page are carried, clickGap apart (bridgeClicks).
//   the world clock, the flight board (keys world, flights) their stop: a person's press that goes in is made on the
//             other side by GET /api/ir/do?fn=ok, a turn inside by fn=cw|ccw, gestureGap apart (the firmware merges
//             closer ones), with X-Twin-Sync: 1 (its gestures are "sync"'s); the press that comes out by a show of the
//             same page (POST /api/panel), which leaves the stop (main.cpp panelShowPage). Each turn there changes a
//             setting (the home city, the airport), compared by the settings round as ever. Not the flight board
//             while the twin holds ZZZZ.
//   the rail board (trains) the same, only while both boards show the same list and knob view (/api/railboard).
//   the media player, the yacht radar, the markets' inner steps: not carried - the player's gestures go to Home
//             Assistant, and would twice; the others keep their own state on each device - said once in the log.
//             Notifications and the 3D scene take the gesture before it is counted (main.cpp): nothing to carry.
// A gesture whose page the other side does not show is not carried (said in the log).
//
// How each thing is written:
//   page      POST /api/panel {"show":{"page":i}}, i looked up by key and name on the target
//             (web_panel.cpp:385-398; indexes differ per device, main.cpp:95-110)
//   style     POST /api/panel {"style":id,"show":{"page":i}}: style is checked and stored 2.5 s later,
//             and moves to the clock page, so show keeps the page (web_panel.cpp:400-402, 465-466)
//   the walk  the panel's step as page and style, to the twin only; a hold of the twin's carousel as a show
//             of the page it is on. The answer of POST /api/panel is the same document as its GET
//             (web_panel.cpp:476-484) and is read as the twin's screen after the write
//   brightness GET /api/display/brightness?value=0..100 - percent, exact both ways; 0 is off
//             (web.cpp:596-609, display.cpp:183-195)
//   on / off  GET /api/display/on | off (web.cpp:582-594)
//   settings  the export's keys through POST /api/import, only those that changed (web.cpp:2499-2761);
//             the rest of the portal's form through POST /save, the whole form - the target's own
//             values with the changed ones replaced, an absent checkbox reads as off, the metric rows
//             and colours ride along or are reset (web.cpp:1537-2264); the zone as a region through
//             /save, a zone that is no region through /api/import. The Panel group's routes as
//             tools/twin/sync.py writes them (pages, carousel, knob, world clock, rail board, flight
//             board, market, media), a remote button's function by GET /api/ir/fn - a "page" button's
//             page by its key and name, since the index differs per device (ir_actions.cpp:132-133) -
//             the log by GET /api/log?on= (web.cpp:191-217, 262-270).
//   effects   the source from GET /api/lua/source?name= where the firmware has it (branch
//             feat/sync-routes, web_panel.cpp handleLuaSource), else the gallery file of firmwareRepo
//             with the same name and size (raw.githubusercontent.com/<repo>/main/gallery/index.json),
//             checked against the index's sha256 where it has one; then POST /api/lua/upload?name=<stem>,
//             multipart, no Origin header (web_panel.cpp:945-963, 1065-1197). Removed: POST /api/lua
//             {"delete":stem} (web_panel.cpp:994-1012). The walk switch: POST /api/lua
//             {"walk":{"i","on","name"}} (web_panel.cpp:976-993). Without /api/lua/source a script is
//             known by its size only, and an edit that keeps the length is not seen: the log says so.
//             An effect is one on both sides by the name its banner shows (a device takes no two scripts
//             whose names read the same), and its file's name is compared exactly: where the two differ
//             only in case (flow.lua on the twin, FLOW.lua on the panel), the twin's file takes the panel's
//             name - removed and uploaded again with its own bytes, its walk switch put back - so its list,
//             in the files' order, is the panel's (caseRenames, renameTwinFiles; the panel's never).
//             A script the other side refuses - its upload answers 400 for the script itself: the checks,
//             the trial run ("too slow for the panel: 4 of 4 frames over 500 ms"), no room - is left out,
//             said once, and the rest goes on: the alignment is done, the direction question lists it as
//             "not carried"; it is sent again only when its content on the source changes (or, refused for
//             room, when the other side has fewer scripts) - refusedFx, kept in sync-state.json.
//   header    every request that writes carries X-Twin-Sync: 1 (Device.send): firmware 2.7.13 puts what it
//             changes down to "sync" in its events, not to a person.
//   firmware  the image of the side that changed, when it fits the target's OTA slot (/api/info
//             otaFreeBytes, compared before anything is read): for the panel, the release of firmwareRepo
//             with that version when it is that very build (the X-App-Elf-Sha256 of HEAD
//             /api/firmware/image equal to the release image's bytes 0xB0-0xCF,
//             esp_app_desc_t.app_elf_sha256) - reading the image out of the panel holds its loop() for
//             the whole transfer - else GET /api/firmware/image, one attempt (feat/sync-routes, web.cpp
//             handleFirmwareImage), checked against X-Firmware-Version, the size /api/info gives, the
//             X-App-Elf-Sha256 it came with and the one HEAD gave; where the firmware has no such route,
//             the release with that version whose OTA_ONLY image has the same size, checked against
//             SHA256SUMS.txt. Always the ESP image's own checks (magic 0xE9, chip id 9, the appended
//             SHA-256, which must be there). An image read is kept (by the source's firmware and ELF
//             SHA-256) until it is confirmed on the target or the source runs another, so a transfer
//             tried again does not read it again. Right before POST /update the pair and the firmware on
//             both sides are read again: for the panel they must be what the person said yes to (Offer),
//             for the twin what the image was read for - else nothing is sent. Then POST /update,
//             multipart (web.cpp:336-393), and the target is watched until /api/info says that version -
//             that build, or for a release that size - settled (not "pending" or "new":
//             update.py:257-264) after a restart, or that it rolled back; five minutes at most. Both
//             sides' bases then take what they run, so a release that stood in for a local build of the
//             same version is not offered back. The build each side should get is kept apart
//             (sync-state.json): a failed transfer to the twin is tried again after 1, 5 and 15 min, and
//             after CARRY_TRIES failures in a row not until "Sync now" (our choice); the question for the
//             panel comes again on "Sync now" and at the next start, as long as they differ.
//
// Never written: /reset (the factory reset: since v2.7.9 only a POST {"confirm":"factory-reset"}, a GET
// is 405 - web.cpp:210-213, 2545-2561), /api/reboot, /api/rename, deviceName, the network fields,
// weatherApiKey (neither compared nor sent: left out of both the export's and the form's keys), the Keys
// page's keys (/api/keys is neither read nor written: not a route of Device.reads or Device.posts),
// metric names, counters, caches, the remote's learned codes. Never
// written to the panel: the settings that describe its own hardware (Settings.hardware) - the
// microphones, the knob, how the presence radar is set up, whether the climate sensor and the remote's
// receiver are used and how the sensor is calibrated - which go from the panel to the twin only. The
// owner's override on the twin: climateHa off (sync.py OVERRIDES), so the twin's indoor sensor is not a
// second device in Home Assistant, and fbAskHa off (firmware 2.7.13), so a twin without an AeroAPI key never
// has HA fetch a flight board with the owner's key; both are left out of what is compared and put back on
// the twin whenever they drift. fbAskHa is also put off at each start of the twin by the app, whatever the
// switch says (app 1.4.1, the owner's item 16: askHaOffAtStart, main.swift holdAskHa). The overrides of
// 2026-09-29 that kept the trains and flights pages out of the twin's walk and its flight board on ZZZZ
// "NO REQUESTS" are gone (the owner, 2026-09-30 19:35): while the twin still
// holds what they left (Settings.residue) and has never been compared on it since, the panel's value goes
// to the twin, whichever way sync aligns, and the airport ZZZZ they added is removed from the twin.
//
// Safety. The panel is a device on the network that answers /api/info with model AnimatedPixelClock,
// is not a twin (a TWIN- name or a MAC 02:54:57:49:*, the twins' locally administered block,
// tools/twin/twin.py) and is not this app's twin (its MAC, its address). A change of more than
// MASS_SETTINGS settings or MASS_DELETES effect removals at once on either side stops the round, and
// the person is asked for the direction instead (a device whose flash was erased looks like that); an
// effects list that cannot be read (/api/lua 404, no "uploaded", LittleFS not mounted) is unknown, not
// empty. Switching sync off (or choosing another panel) stops every write at once: each request that
// writes asks first, and one that is under way - an upload, the firmware - is cut off; so is a read of
// the sync routes (a script's source, the firmware image), which holds the panel's loop() while it runs.
// A new pair, or the pair changing (a MAC that is not the pair's), takes both confirmations again.
//
// Settings (defaults): syncEnabled (off by default; the switch - `defaults write <bundle id> syncEnabled
// -bool NO` from a terminal switches it too, at once, whatever window is open), panelAddress (host[:port];
// empty: found over mDNS, and looked for again when the panel stops answering), panelMac (the panel picked
// from the found ones), firmwareRepo (owner/name, default NickoScope/AnimatedPixelClock),
// syncSettingsEveryS (60, at least 15).
// For tests only: honoured only when the "panel" has a twin's MAC (02:54:57:49:*) and panelAddress names
// it by a loopback address - 127.0.0.1 (localhost is one too, but since v2.7.9 /api/export, /api/portal
// and /api/market answer 403 to a Host that is neither an IPv4 address nor a .local name, web.cpp:2510-2523,
// 1144, 2584, web_panel.cpp:853, so sync cannot read settings through it) - a real panel has neither,
// whatever it is called -
// and syncPanelMayBeTwinForTesting (accept a twin as the panel) is on: syncConsentForTesting (answer
// "Enable sync with the panel?" Enable by itself), syncAlignForTesting ("panel" | "twin": answer the
// direction question by itself - "twin" also carries the twin's changes at a resume, "panel" gives the
// twin the panel's state back), syncAutoConfirmForTesting (answer both firmware windows yes by itself),
// syncAnswerDelayForTesting (seconds before these answers), syncEnableReturnForTesting (1: press Return
// in "Enable sync with the panel?"; 2: press its Enable, then Return in the alignment window - as a
// person would by accident), syncPressReturnForTesting (1: Return in "Update the panel too?"; 2: its
// Update, then Return in "Really flash the physical panel?"), syncImageHoldForTesting (seconds to wait
// after the image is read, before the last look and POST /update). All off by default.

import AppKit
import CryptoKit
import Foundation
import LocalAuthentication
import Security

// MARK: - words

/// A message in both languages; rendered when shown, so the EN · RU switch retranslates it.
struct Msg { let en: String, ru: String; func text(_ lang: String) -> String { lang == "ru" ? ru : en } }
func M(_ en: String, _ ru: String) -> Msg { Msg(en: en, ru: ru) }

struct SyncError: Error { let msg: Msg; init(_ m: Msg) { msg = m } }
/// A device does not answer at all: said once, not every round.
struct SyncDown: Error { let side: Side; let msg: Msg }
/// Not an error: the engine is waiting (no panel yet, the twin is starting, a question is open).
struct SyncWait: Error { let msg: Msg; init(_ m: Msg) { msg = m } }
/// Sync was switched off, or the pair is being changed, while something was to be written: nothing more is.
struct SyncStopped: Error {}
/// A script's SHA-256 read on its schedule was cut off: a person's change came meanwhile, and goes first. The read is
/// made again a few seconds later (SyncEngine.hashTick), never at once.
struct Yielded: Error {}
/// A side restarted: a read of its uptime (/api/status or /api/info) came back smaller than the one before.
/// Whatever the round was about to do was decided on the side as it was before, so the round stops there,
/// and the restart is dealt with first (SyncEngine.restarts).
struct SyncRestart: Error { let side: Side }
/// GET /api/lua/source?name= answered 404 for a script GET /api/lua lists (the panel, 30.09 21:23-22:58:
/// LA_GIOCONDA, "no uploaded script by that name", every round).
struct NoSuchScript: Error { let side: Side, stem: String, why: String }
/// POST /api/lua/upload answered 400 for the script itself: its checks (lua_store.cpp luaStoreValidate), its
/// trial run (lua_effects.cpp runTrial: "does not load", "frame N fails", "too slow for the panel"), a name that
/// reads as another effect's - or the device has no room for it (ROOM: all slots used, the filesystem full).
struct UploadRefused: Error { let why: String, room: Bool }

enum Side: String, Codable {
    case panel, twin
    var other: Side { self == .panel ? .twin : .panel }
    var word: Msg { self == .panel ? M("the panel", "панель") : M("the twin", "двойник") }
    /// "на панели" / "на двойнике", "у панели" / "у двойника", "панели" / "двойника".
    var on: String { self == .panel ? "на панели" : "на двойнике" }
    var at: String { self == .panel ? "у панели" : "у двойника" }
    var of: String { self == .panel ? "панели" : "двойника" }
    var to: String { self == .panel ? "панель" : "двойника" }
    var its: String { self == .panel ? "её" : "его" }
    var arrow: String { self == .panel ? "panel→twin" : "twin→panel" }
}
let sides: [Side] = [.panel, .twin]

// MARK: - JSON helpers

typealias J = [String: Any]
extension Dictionary where Key == String, Value == Any {
    func s(_ k: String) -> String? { self[k] as? String }
    func i(_ k: String) -> Int? { (self[k] as? NSNumber)?.intValue }
    func b(_ k: String) -> Bool? { (self[k] as? NSNumber)?.boolValue }
    func o(_ k: String) -> J? { self[k] as? J }
    func a(_ k: String) -> [J] { self[k] as? [J] ?? [] }
}
func isBool(_ v: Any?) -> Bool { if let n = v as? NSNumber { return CFGetTypeID(n) == CFBooleanGetTypeID() }; return false }
/// One text for a JSON value, the same for equal values: keys sorted.
func canon(_ v: Any?) -> String {
    guard let v, !(v is NSNull) else { return "null" }
    if let d = try? JSONSerialization.data(withJSONObject: v, options: [.sortedKeys, .fragmentsAllowed, .withoutEscapingSlashes]) {
        return String(decoding: d, as: UTF8.self)
    }
    return "\(v)"
}
func hex<S: Sequence>(_ b: S) -> String where S.Element == UInt8 { b.map { String(format: "%02x", $0) }.joined() }
func sha256Hex(_ d: Data) -> String { hex(SHA256.hash(data: d)) }

/// The key of the digests in sync-state.json: 32 random bytes in the login Keychain, never in the file,
/// so a value cannot be found from its digest by trying the likely ones (a brightness, a sum of money).
/// Where the Keychain does not give it (locked, or a build signed otherwise), a key of this run only:
/// then nothing is kept between runs, and each start asks the direction.
enum StateKey {
    /// One key per bundle identifier: a test build never reads or writes the installed app's.
    static let service = (Bundle.main.bundleIdentifier ?? "") + ".sync-state"
    static let account = "hmac-key"
    private static let loaded: (key: SymmetricKey, kept: Bool) = load()
    static var key: SymmetricKey { loaded.key }
    static var kept: Bool { loaded.kept }
    /// Which key a file was written with: not the key, a digest of a fixed text under it.
    static var tag: String { hex(HMAC<SHA256>.authenticationCode(for: Data("sync-state.json".utf8), using: key).prefix(6)) }

    private static func load() -> (key: SymmetricKey, kept: Bool) {
        // Not an app bundle (a unit test): the Keychain is not touched at all.
        guard Bundle.main.bundleIdentifier != nil else { return (SymmetricKey(size: .bits256), false) }
        let ctx = LAContext(); ctx.interactionNotAllowed = true        // never a dialog from the sync thread
        let find: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                   kSecAttrAccount as String: account, kSecReturnData as String: true,
                                   kSecMatchLimit as String: kSecMatchLimitOne, kSecUseAuthenticationContext as String: ctx]
        var out: CFTypeRef?
        let st = SecItemCopyMatching(find as CFDictionary, &out)
        if st == errSecSuccess, let d = out as? Data, d.count == 32 { return (SymmetricKey(data: d), true) }
        if st == errSecItemNotFound {
            var bytes = [UInt8](repeating: 0, count: 32)
            if SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess {
                let d = Data(bytes)
                let add: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                          kSecAttrAccount as String: account, kSecValueData as String: d,
                                          kSecAttrLabel as String: "TWIN-NickoScopeMatrix-64x128 sync state",
                                          kSecAttrDescription as String: "the key of the digests in sync-state.json",
                                          kSecUseAuthenticationContext as String: ctx]
                if SecItemAdd(add as CFDictionary, nil) == errSecSuccess { return (SymmetricKey(data: d), true) }
            }
        }
        return (SymmetricKey(size: .bits256), false)
    }
}
/// What a base keeps of a value: its HMAC under StateKey - equality is all a base is for.
func digest(_ v: Any?) -> String { hex(HMAC<SHA256>.authenticationCode(for: Data(canon(v).utf8), using: StateKey.key).prefix(12)) }
func jsonData(_ o: Any) throws -> Data { try JSONSerialization.data(withJSONObject: o, options: [.withoutEscapingSlashes]) }
func formText(_ v: Any) -> String { (v as? NSNumber)?.stringValue ?? (v as? String) ?? "\(v)" }
/// "my_fx" -> "MY FX": the name the banner shows and the walk switch is kept by (lua_store.cpp:49-53).
func shownName(_ stem: String) -> String { String(stem.map { $0 == "_" ? " " : Character($0.uppercased()) }) }
func normMac(_ m: String?) -> String { (m ?? "").trimmingCharacters(in: .whitespacesAndNewlines).uppercased().replacingOccurrences(of: "-", with: ":") }
/// The twins' block: locally administered, "TWI" (tools/twin/twin.py MAC; main.swift Twin.prepare).
func twinMacLike(_ mac: String) -> Bool { normMac(mac).hasPrefix("02:54:57:49:") }
func twinLike(name: String, mac: String) -> Bool { name.hasPrefix("TWIN-") || twinMacLike(mac) }
/// host or IPv4, with an optional port: nothing that could carry a path or a query onto a device.
func validAddress(_ a: String) -> Bool { a.range(of: #"^[A-Za-z0-9.-]{1,253}(:[0-9]{1,5})?$"#, options: .regularExpression) != nil }
func validRepo(_ r: String) -> Bool { r.range(of: #"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9_.-]{1,100}$"#, options: .regularExpression) != nil }
/// 127.0.0.1 or localhost, with or without a port: this Mac itself.
func isLoopback(_ a: String) -> Bool {
    let host = a.split(separator: ":", maxSplits: 1).first.map(String.init) ?? a
    return host == "127.0.0.1" || host.lowercased() == "localhost"
}
/// esp_app_desc_t.app_elf_sha256: at 0xB0 of an application image (24-byte image header, 8-byte segment
/// header, then the descriptor, whose field is at 144 - esptool --elf-sha256-offset 0xb0).
func appElfSha256(_ d: Data) -> String? { d.count >= 0xD0 ? hex([UInt8](d)[0xB0 ..< 0xD0]) : nil }
/// "2.7.8" < "2.7.10": the version as numbers; nil when it is not three of them.
func versionNumbers(_ v: String) -> [Int]? {
    let t = v.hasPrefix("v") ? String(v.dropFirst()) : v
    let p = t.split(separator: ".").prefix(3).map { Int($0.prefix { $0.isNumber }) ?? -1 }
    return p.count == 3 && !p.contains(-1) ? p : nil
}

// MARK: - one device over HTTP

struct Answer {
    let status: Int, data: Data, headers: [String: String]
    var text: String { String(decoding: data, as: UTF8.self) }
    var object: J? { try? JSONSerialization.jsonObject(with: data) as? J }
    func header(_ n: String) -> String? { headers.first { $0.key.caseInsensitiveCompare(n) == .orderedSame }?.value }
    /// The firmware's own words for a refusal: {"success":false,"error":...} or "message".
    var why: String { let o = object; return String((o?.s("error") ?? o?.s("message") ?? text).prefix(160)) }
    /// Seconds from a Retry-After header, when there is one.
    var retryAfter: Double? { header("Retry-After").flatMap { Double($0.trimmingCharacters(in: .whitespaces)) } }
}

final class Device {
    let side: Side, address: String
    /// The least time between two requests to the panel, so polling never runs back to back on it. Kept
    /// before the next request to it, not after each one: a write to the twin that follows a read of the
    /// panel does not wait for it.
    var pace: TimeInterval
    private var lastEnd = Date.distantPast
    /// Asked before every request that writes, and while it runs: false stops it (sync switched off, the
    /// pair being changed) - nothing more is sent, and a request under way is cut off.
    var mayWrite: () -> Bool = { true }
    /// Told before every request that writes is sent (the twin's: a write may hold its carousel - sync's
    /// hold, not a person's; SyncEngine.sawTwin).
    var willWrite: () -> Void = {}
    /// Called before every request, before its pace is kept: inside a round that takes a while, what came meanwhile
    /// (the instant events) is served here, between any two of its requests (LongRounds.between). What it throws
    /// stops the request: nothing is sent.
    var before: () throws -> Void = {}
    /// A read of a round that takes a while gives way while this is true (SyncEngine.yieldNow: a person's change, or the
    /// carousel's step, came from this very side - it goes to the other one, which is free): it is cut off, what came is
    /// served (before), and it is sent again - maxYields times at most, then read whole. So a person's change waits for no
    /// read of such a round (GET /api/lua: 0.7 s on the panel, 1.1 s on the twin). Never a write (it may have landed),
    /// never the firmware image (one attempt: the whole transfer). Looked at every 50 ms while a read runs.
    var yields: () -> Bool = { false }
    static let maxYields = 1

    init(_ side: Side, _ address: String) { self.side = side; self.address = address; pace = side == .panel ? Device.panelPace : 0 }
    static let panelPace: TimeInterval = 0.15

    static let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.httpMaximumConnectionsPerHost = 1
        c.requestCachePolicy = .reloadIgnoringLocalCacheData
        c.urlCache = nil; c.httpCookieStorage = nil; c.httpShouldSetCookies = false
        return URLSession(configuration: c)
    }()

    // GET of these only reads, without a query (sync.py READ_ROUTES; the Panel group writes only in
    // POST, web_panel.cpp:381-968). No other GET is sent but the actions below.
    static let reads: Set<String> = ["/api/info", "/api/status", "/api/panel", "/api/portal", "/api/export", "/api/knob", "/api/lua",
                                     "/api/worldclock", "/api/railboard", "/api/flightboard", "/api/market", "/api/media", "/api/ir"]
    static let posts: Set<String> = ["/save", "/api/import", "/api/panel", "/api/knob", "/api/lua", "/api/worldclock", "/api/railboard",
                                     "/api/flightboard", "/api/market", "/api/media", "/api/sync/listen"]
    // The GETs that change something, each with the only parameters it may carry. /api/ir/do: a gesture inside a page
    // (fn ok, cw, ccw), as the remote's button would make it - a person's, carried (SyncEngine.replay).
    static let actions: [String: Set<String>] = ["/api/display/on": [], "/api/display/off": [], "/api/display/brightness": ["value"],
                                                 "/api/ir/fn": ["btn", "fn", "page"], "/api/log": ["on"], "/api/ir/do": ["fn"]]
    /// The sync routes (feat/sync-routes). GET|HEAD /api/firmware/image answers only a request with
    /// X-Twin-Sync: 1, else 403 before the image is touched (web.cpp handleFirmwareImage) - a header no web
    /// page can send to another address without a preflight, which the firmware does not answer. GET
    /// /api/lua/source needs no header and answers any page, with Access-Control-Allow-Origin: *
    /// (web_panel.cpp handleLuaSource: the twin's panel page reads a script's header from it). The header
    /// is sent on both. Each read of them holds the panel's loop() while it runs, so switching sync off
    /// cuts them off as it cuts off a write, and none is started while it is off.
    /// Every request that writes carries X-Twin-Sync: 1 as well - each POST (/save, /api/import, /api/panel,
    /// /api/lua, /api/lua/upload, /update, ...) and each GET that acts (/api/display/*, /api/ir/fn, /api/log):
    /// firmware 2.7.13 puts what such a request changes down to "sync" in its events and in /api/panel?input=
    /// (sync_events.cpp syncHttpBy, syncAfterHttp), so sync's own writes are told from a person's. The reads
    /// carry nothing: the firmware counts no read as a cause (syncAfterHttp).
    static let syncRoutes: Set<String> = ["/api/lua/source", "/api/firmware/image"]

    func writes(_ method: String, _ path: String) -> Bool { !(method == "GET" || method == "HEAD") || Device.actions[path] != nil }
    /// Stopped by the switch: every write, and the reads of the sync routes.
    func stoppable(_ method: String, _ path: String) -> Bool { writes(method, path) || Device.syncRoutes.contains(path) }

    /// One request. 503 with Retry-After is asked again (the firmware stood aside before doing anything);
    /// 503 without it is the answer. A transport fault is retried for a request that only reads, up to
    /// ATTEMPTS in all - a POST may have landed. QUIET: a write that changes nothing on the screen (the events'
    /// subscription) - willWrite is not told; UNSTOPPABLE: sent whatever the switch says (only the subscription's
    /// end, POST /api/sync/listen {"stop":true}, which stops the device sending). A read gives way (yields); YIELD, a read
    /// on a schedule that gives way of its own: true cuts it off, and it is not sent again here (Yielded).
    func send(_ method: String, _ path: String, query: [(String, String)] = [], body: Data? = nil, type: String? = nil,
              timeout: TimeInterval = 15, limit: Int = 2 << 20, attempts: Int = 3, quiet: Bool = false, unstoppable: Bool = false,
              yield: (() -> Bool)? = nil) throws -> Answer {
        var c = URLComponents(string: "http://\(address)\(path)")!
        if !query.isEmpty { c.queryItems = query.map { URLQueryItem(name: $0.0, value: $0.1) } }
        guard let url = c.url else { throw SyncError(M("bad address \(address)", "неверный адрес \(address)")) }
        let write = writes(method, path), stop = !unstoppable && stoppable(method, path), allowed = mayWrite
        let gives = !write && path != "/api/firmware/image", y = yields
        var attempt = 0, yielded = 0
        while true {
            attempt += 1
            try before()
            if stop && !allowed() { throw SyncStopped() }
            if write && !quiet { willWrite() }
            var req = URLRequest(url: url, timeoutInterval: timeout)
            req.httpMethod = method; req.httpBody = body
            if let type { req.setValue(type, forHTTPHeaderField: "Content-Type") }
            if write || Device.syncRoutes.contains(path) { req.setValue("1", forHTTPHeaderField: "X-Twin-Sync") }
            if pace > 0 { let wait = lastEnd.addingTimeInterval(pace).timeIntervalSinceNow; if wait > 0 { Thread.sleep(forTimeInterval: wait) } }
            let mayYield = gives && (yield != nil || yielded < Device.maxYields)
            let cut: () -> Bool = { yield?() ?? y() }
            let cancel: (() -> Bool)? = stop || mayYield ? { (stop && !allowed()) || (mayYield && cut()) } : nil
            let (a, err, stopped) = Device.exchange(req, limit: limit, cancel: cancel, every: mayYield ? 0.05 : 0.2)
            lastEnd = Date()
            if stopped {
                if stop && !allowed() { throw SyncStopped() }
                if yield != nil { throw Yielded() }                  // a read on a schedule: made again later, not now
                yielded += 1; attempt -= 1; continue                 // gave way: what came is served (before), and it goes again
            }
            if let a, a.status == 503, let wait = a.retryAfter, attempt < 6 {
                Thread.sleep(forTimeInterval: min(wait, 30) + 0.4 * Double(attempt)); continue
            }
            if let a { return a }
            if !write, attempt < attempts { Thread.sleep(forTimeInterval: 0.8); continue }
            throw SyncDown(side: side, msg: M("\(side.word.en) at \(address) does not answer (\(err ?? "?"))",
                                              "\(side.word.ru) по адресу \(address) не отвечает (\(err ?? "?"))"))
        }
    }

    /// The request, waited for in steps of EVERY seconds: CANCEL true cuts it off (the third value says so).
    static func exchange(_ req: URLRequest, limit: Int, cancel: (() -> Bool)? = nil, every: TimeInterval = 0.2) -> (Answer?, String?, Bool) {
        let sem = DispatchSemaphore(value: 0)
        var out: (Answer?, String?) = (nil, "no answer")
        let task = session.dataTask(with: req) { d, r, e in
            if let e { out = (nil, e.localizedDescription) }
            else if let h = r as? HTTPURLResponse {
                let data = d ?? Data()
                if data.count > limit { out = (nil, "larger than \(limit) bytes") }
                else {
                    var hs: [String: String] = [:]
                    for (k, v) in h.allHeaderFields { hs["\(k)"] = "\(v)" }
                    out = (Answer(status: h.statusCode, data: data, headers: hs), nil)
                }
            }
            sem.signal()
        }
        task.resume()
        var stopped = false
        while sem.wait(timeout: .now() + every) == .timedOut {
            if !stopped, let cancel, cancel() { stopped = true; task.cancel() }
        }
        return stopped ? (nil, "stopped", true) : (out.0, out.1, false)
    }

    func refused(_ what: String, _ a: Answer) -> SyncError {
        SyncError(M("\(side.word.en): \(what): HTTP \(a.status) \(a.why)", "\(side.word.ru): \(what): HTTP \(a.status) \(a.why)"))
    }

    /// A read route's document; nil when this build has no such route (404).
    func get(_ path: String) throws -> J? {
        precondition(Device.reads.contains(path), "GET \(path) is not a route that only reads")
        let a = try send("GET", path)
        if a.status == 404 { return nil }
        guard a.status == 200 else { throw refused("GET \(path)", a) }
        guard let o = a.object else { throw SyncError(M("\(side.word.en): GET \(path): not JSON", "\(side.word.ru): GET \(path): не JSON")) }
        return o
    }

    /// GET /api/panel, with ?input=N where the firmware keeps the gestures (2.7.13: now.inputSeq; the ones after
    /// seq N come in "input", web_panel.cpp buildPanel -> syncPanelJson). nil: no such route.
    func panelDoc(input: Int?) throws -> J? {
        let a = try send("GET", "/api/panel", query: input.map { [("input", "\($0)")] } ?? [])
        if a.status == 404 { return nil }
        guard a.status == 200 else { throw refused("GET /api/panel", a) }
        guard let o = a.object else { throw SyncError(M("\(side.word.en): GET /api/panel: not JSON", "\(side.word.ru): GET /api/panel: не JSON")) }
        return o
    }

    @discardableResult
    func post(_ path: String, _ obj: J) throws -> J {
        precondition(Device.posts.contains(path) && path != "/save", "POST \(path) is not a route sync writes")
        let data = try jsonData(obj)
        // The Panel group takes a body of 1024 bytes, /api/market 8192 (web_panel.cpp:136, 841).
        let cap = path == "/api/market" ? 8192 : path == "/api/import" ? Int.max : 1024
        guard data.count <= cap else { throw SyncError(M("POST \(path): \(data.count) bytes, the route takes \(cap)", "POST \(path): \(data.count) байт, маршрут берёт \(cap)")) }
        let a = try send("POST", path, body: data, type: "application/json", timeout: 30)
        guard a.status == 200 else { throw refused("POST \(path)", a) }
        let o = a.object ?? [:]
        if o.b("success") == false { throw refused("POST \(path)", a) }
        return o
    }

    /// POST /save: application/x-www-form-urlencoded, every pair parsed (arduino-esp32 WebServer).
    func save(_ pairs: [(String, String)]) throws {
        var allowed = CharacterSet.alphanumerics; allowed.insert(charactersIn: "-._~")
        let body = pairs.map { "\($0.0.addingPercentEncoding(withAllowedCharacters: allowed)!)=\($0.1.addingPercentEncoding(withAllowedCharacters: allowed)!)" }
            .joined(separator: "&")
        let a = try send("POST", "/save", body: Data(body.utf8), type: "application/x-www-form-urlencoded", timeout: 30)
        guard a.status == 200, a.object?.b("success") != false else { throw refused("POST /save", a) }
    }

    func action(_ path: String, _ params: [(String, String)] = []) throws {
        guard let ok = Device.actions[path], params.allSatisfy({ ok.contains($0.0) }) else {
            preconditionFailure("GET \(path) is not an action sync sends")
        }
        let a = try send("GET", path, query: params)
        guard a.status == 200 else { throw refused("GET \(path)", a) }
    }

    /// POST of one file, multipart. No Origin header: the Lua upload refuses a foreign one
    /// (web_panel.cpp:945-963), and URLSession sends none.
    func upload(_ path: String, query: [(String, String)], field: String, filename: String, data: Data, timeout: TimeInterval) throws -> Answer {
        let b = "----twinsync" + UUID().uuidString.replacingOccurrences(of: "-", with: "")
        var body = Data("--\(b)\r\nContent-Disposition: form-data; name=\"\(field)\"; filename=\"\(filename)\"\r\nContent-Type: application/octet-stream\r\n\r\n".utf8)
        body.append(data); body.append(Data("\r\n--\(b)--\r\n".utf8))
        return try send("POST", path, query: query, body: body, type: "multipart/form-data; boundary=\(b)", timeout: timeout)
    }

    /// GET /api/lua/source with no argument answers 400 where the route exists, 404 where it does not
    /// (handleLuaSource: "send name=<script> or i=<effect index>"; the WebServer's own 404 otherwise).
    /// nil: not known now (503, no answer, a refusal) - asked again next time, never remembered.
    func probeLuaSource() -> Bool? {
        guard let a = try? send("GET", "/api/lua/source", attempts: 1) else { return nil }
        return a.status == 400 ? true : a.status == 404 ? false : nil
    }
    /// HEAD /api/firmware/image: 200 with the build's ELF SHA-256, 404 where the route is not (a build
    /// with a Wi-Fi password compiled in has none). nil: not known now.
    func probeImage() -> (has: Bool, elf: String?)? {
        guard let a = try? send("HEAD", "/api/firmware/image", attempts: 1) else { return nil }
        if a.status == 200 { return (true, a.header("X-App-Elf-Sha256")?.lowercased()) }
        return a.status == 404 ? (false, nil) : nil
    }

    /// A script's bytes by its file's name - exactly as GET /api/lua lists it (uploaded.scripts[].name), never
    /// the name its banner shows. A 404 for a script the list has (NoSuchScript): the device will not give it out.
    /// YIELD: a read on the schedule (SyncEngine.hashTick), cut off when it turns true (Yielded).
    func luaSource(_ stem: String, yield: (() -> Bool)? = nil) throws -> Data {
        let a = try send("GET", "/api/lua/source", query: [("name", stem)], timeout: 30, limit: 1 << 20, yield: yield)
        if a.status == 404 { throw NoSuchScript(side: side, stem: stem, why: a.why) }
        guard a.status == 200, a.header("X-Lua-Name") == stem else { throw refused("GET /api/lua/source?name=\(stem)", a) }
        return a.data
    }
    /// The running image: one attempt, never repeated - the panel's loop() is inside the handler for
    /// the whole transfer (web.cpp handleFirmwareImage, up to 120 s).
    func firmwareImage() throws -> Answer {
        let a = try send("GET", "/api/firmware/image", timeout: 180, limit: 8 << 20, attempts: 1)
        guard a.status == 200 else { throw refused("GET /api/firmware/image", a) }
        return a
    }
}

/// The rounds that take a while (SyncEngine: effects, settings, firmware, a script's SHA-256 on its schedule) and what is
/// served between any two of their requests: each Device's `before` calls between(), which, inside such a round (run) and
/// not inside a serving itself, calls serve (SyncEngine.serveEvents: the instant events that came meanwhile, carried now).
/// A serving's own requests go through between() too, and are not served again (no recursion).
final class LongRounds {
    private(set) var depth = 0, serving = false
    var serve: () throws -> Void = {}
    func run<T>(_ f: () throws -> T) rethrows -> T { depth += 1; defer { depth -= 1 }; return try f() }
    func between() throws {
        guard depth > 0, !serving else { return }
        serving = true; defer { serving = false }
        try serve()
    }
}

// MARK: - what a device holds

/// now.fx (firmware 2.7.13, sync_events.cpp syncPanelJson; lua_effects.cpp luaEffectsFx): the Lua effect selected on
/// this device - its index here, -1 for none - whether its first frame has come (open), how many times an effect was
/// chosen or reopened (run), and the clicks it has had since it was chosen (clicks: its button, px.button - the knob's
/// click, the remote's OK, POST /api/lua {"click":true}).
struct Fx: Equatable { var id = -1, open = false, run = 0, clicks = 0 }

struct Screen {
    var key = "", name = "", page = 0, style = 0, bright = 0, off = false, uptime = 0
    var running = false, allStyles = false, pageS = 0
    /// The carousel switched on (its setting: walking, or held after a person's choice), its idle time, the
    /// seconds it is still held (rounded up), the seconds to its next step, when it is on and the page has a
    /// time (web_panel.cpp:345-365: max(secs - pageS, holdS), pageS rounded down); a page entered with the
    /// knob (now.entered).
    var enabled = false, idleS = 0, holdS = 0, nextS: Int? = nil, entered = false
    var pages: [J] = [], styles: Set<Int> = []
    /// Firmware 2.7.13: the effect on screen (nil where the firmware has no now.fx), the number of its last event
    /// (now.seq) and of its last gesture (now.inputSeq), a notification over the page (now.notify), and the gestures
    /// GET /api/panel?input=N gave (oldest first: seq, epochMs, kind, by, page, entered).
    var fx: Fx?, seq: Int?, inputSeq: Int?, notify = false, inputs: [J] = []
    /// What GET /api/panel says - and the answer to POST /api/panel, the same document (handlePanel,
    /// web_panel.cpp:476-484): the page, the style and the carousel. Brightness, on/off and uptime come
    /// from /api/status and are left as they are.
    mutating func apply(panel pn: J) {
        let now = pn.o("now") ?? [:], car = pn.o("carousel") ?? [:]
        key = now.s("key") ?? ""; name = now.s("name") ?? ""; page = now.i("page") ?? 0; style = now.i("style") ?? 0
        entered = now.b("entered") ?? false
        running = car.b("running") ?? false; allStyles = car.b("allStyles") ?? false; pageS = car.i("pageS") ?? 0
        enabled = car.b("enabled") ?? false; idleS = car.i("idleS") ?? 0; holdS = car.i("holdS") ?? 0; nextS = car.i("nextS")
        pages = pn.a("pages"); styles = Set(pn.a("styles").compactMap { $0.i("id") })
        notify = now.b("notify") ?? false
        fx = now.o("fx").map { Fx(id: $0.i("id") ?? -1, open: $0.b("open") ?? false, run: $0.i("run") ?? 0, clicks: $0.i("clicks") ?? 0) }
        seq = now.i("seq"); inputSeq = now.i("inputSeq"); inputs = pn.a("input")
    }
    /// The page with this index, as key and name.
    func page(_ i: Int) -> (key: String, name: String)? {
        pages.first { $0.i("i") == i }.map { ($0.s("key") ?? "", $0.s("name") ?? "") }
    }
    /// What is mirrored as "the page": its key and name (indexes differ per device). Cards are this
    /// device's own notifications and stay out.
    var shown: String { key == "cards" ? "cards" : "\(key)/\(name)" }
    var fields: [String: String] { ["page": shown, "style": "\(style)", "bright": "\(bright)", "off": off ? "1" : "0"] }
    /// The fields the carousel walks right now (carousel.running, carousel.cpp:25-27): the page, and with "walk
    /// every clock style" the style too - each style a slot of its own, and the first style put back when the
    /// lap ends (main.cpp:1173-1180; clockStyleCarouselNext, clock_style.cpp:64-78). The carousel only shows a
    /// style (showStyle, clock_style.cpp:52-62: not saved); nobody chose it. So these are never a person's
    /// change of this side's. The leading panel's walk is what the twin is given (SyncEngine.followFields);
    /// the twin's own walk is never written to the panel: there a style is saved to NVS as a person's choice
    /// 2.5 s later ("[style] saved style N", clock_style.cpp:136-142) and the panel's carousel is held for the
    /// idle time (panelShowStyle -> panelShowPage -> carouselNote, main.cpp:883-909).
    var walked: Set<String> { running ? (allStyles ? ["page", "style"] : ["page"]) : [] }
    /// The Lua pages and their walk switches: when this changes, the effects are read.
    var luaSig: String {
        pages.filter { $0.s("key") == "lua" && !($0.s("name") ?? "").isEmpty }
            .map { "\($0.s("name") ?? "")=\($0.b("effectOn") ?? true)" }.joined(separator: ",")
    }
    func index(key: String, name: String) -> Int? { pages.first { $0.s("key") == key && $0.s("name") == name }?.i("i") }
    /// The effect the page shown opens: the Lua pages are one run of indexes, each listed with key "lua", slots without a
    /// script too, and page i opens effect i - PAGE_LUA_FIRST (main.cpp ctrlLuaEffect, panelPageKey). Nil off a Lua page.
    var fxIndex: Int? {
        guard key == "lua", let first = pages.filter({ $0.s("key") == "lua" }).compactMap({ $0.i("i") }).min() else { return nil }
        return page - first
    }
    /// now.fx when it is the effect of the page shown (fx.id is that page's effect), else nil. The device selects a page's
    /// effect in the pass of loop() after the page changed (main.cpp:1214 luaEffectsSelect): until then - the answer to POST
    /// /api/panel {"show"}, the datagram of the page change - now.fx is still the effect before, its run and its clicks
    /// (2026-10-01, the review's C5: page AUTUMN SLOW with fx {id 2 = AAA NEW, run 19, clicks 3}).
    var fxHere: Fx? { fx.flatMap { $0.id >= 0 && $0.id == fxIndex ? $0 : nil } }
}

struct Effects {
    var bytes: [String: Int] = [:]      // shown name -> size of the uploaded script
    var hash: [String: String] = [:]    // shown name -> SHA-256 of its bytes, where the device gives them out
    var stem: [String: String] = [:]    // shown name -> its file's stem on this device
    var names: [String] = []            // effects[], in order: the index for walk
    var files: [String] = []            // uploaded.scripts[].name, in the device's order
    var walk: [String: Bool] = [:]
    var byHash = false                  // compared by content (GET /api/lua/source), else by size
    /// Compared by content, and no SHA-256 known for the size it has now (new, or its size changed): its read is
    /// due first (SyncEngine.hashNext); until then it is not compared (compared(base:)).
    var unknown: Set<String> = []
    var fields: [String: String] {
        var f = ["mode": byHash ? "hash" : "size"]
        for (n, b) in bytes { f["s." + n] = hash[n].map { "h" + $0 } ?? "\(b)"; f["w." + n] = walk[n] == false ? "0" : "1" }
        return f
    }
    /// The fields as compared with the base B: an effect whose content is not known yet keeps the base's value - not a
    /// change yet, nor a removal; one new to the list is not there yet, its walk switch either. Once its SHA-256 is
    /// read, it is compared as ever.
    func compared(base b: [String: String]?) -> [String: String] {
        var f = fields
        for n in unknown {
            f["s." + n] = b?["s." + n]
            if b?["s." + n] == nil { f["w." + n] = nil }
        }
        return f
    }
    /// Whether effect N is the same script on both: by content where both know it, else by size.
    static func same(_ a: Effects, _ b: Effects, _ n: String) -> Bool {
        guard let x = a.bytes[n], let y = b.bytes[n] else { return a.bytes[n] == nil && b.bytes[n] == nil }
        if let h = a.hash[n], let g = b.hash[n] { return h == g }
        return x == y
    }
}

struct Firmware {
    var version = "", build = "", bytes = 0, otaFree = 0, state = "", rolledBackFrom = "", mac = "", name = "", model = ""
    var uptime = 0, webRefused = 0, allocFails = 0
    init() {}
    init(_ i: J) {
        version = i.s("version") ?? ""; build = i.s("build") ?? ""; bytes = i.i("firmwareBytes") ?? 0
        otaFree = i.i("otaFreeBytes") ?? 0; mac = normMac(i.s("mac")); name = i.s("deviceName") ?? ""; model = i.s("model") ?? ""
        uptime = i.i("uptime") ?? 0; webRefused = i.i("webRefused") ?? 0; allocFails = i.i("allocFails") ?? 0
        let ota = i.o("ota") ?? [:]
        state = ota.s("state") ?? ""; rolledBackFrom = ota.s("rolledBackFrom") ?? ""
    }
    /// An image is known by version, build time and size: /api/info gives out no hash of it.
    var id: String { version.isEmpty ? "" : "\(version) (\(build), \(bytes) B)" }
    /// A new image confirms itself or rolls back: until then "pending" or "new". Every other state -
    /// "valid", "undefined" (flashed over USB: no OTA state), "unknown" - is where it stays
    /// (boot_health.cpp:36-45; tools/agent/update.py:257-264 waits for the same two).
    var settled: Bool { state != "pending" && state != "new" }
}

// MARK: - the settings as one flat dictionary (tools/twin/sync.py settings_of, both ways)

enum Settings {
    // Not compared: identity, secrets, state, what the screen mirror carries, the zone (compared as
    // "tz"), the owner's overrides (climateHa, fbAskHa).
    static let exportSkip: Set<String> = ["deviceName", "weatherApiKey", "metricNames", "clockStyle", "timezoneString", "gmtOffset",
                                          "daylightSaving", "climateHa", "fbAskHa", "ntpServer1", "ntpServer2"]   // NTP: the network's, not carried
    // The form's own: identity and the network (a useStaticIP change restarts, web.cpp:2251-2263), the
    // key, what the screen mirror carries, the zone's region, a card marker, and the export's keys
    // under other names (rowMode = displayRowMode, rpmKFormat = useRpmKFormat, netMBFormat =
    // useNetworkMBFormat, weatherFahrenheit = weatherUseFahrenheit; web.cpp:1599-1607, 1713).
    static let formIdentity: Set<String> = ["deviceName", "useStaticIP", "staticIP", "gateway", "subnet", "dns1", "dns2"]
    static let formSkip: Set<String> = formIdentity.union(["ntpServer1", "ntpServer2", "weatherApiKey", "displayBrightness", "clockStyle", "timezoneRegion", "irCard",
                                                           "rowMode", "rpmKFormat", "netMBFormat", "weatherFahrenheit"])
    // Not a page switch of its own: the cards (their switch is cardsOn, "pages.cards") and the clock, which
    // cannot be switched off. The flights and trains pages are mirrored like the rest (the owner, 2026-09-30 19:35).
    static let pageSkip: Set<String> = ["cards", "clock"]
    /// What the carousel is, as settings: its own (on/off, idle, slot, every clock style) and which pages it
    /// visits. Read in every screen round (GET /api/panel) and carried at once: one carousel for both sides.
    /// The effects' walk switches are the effects' (Effects.walk), read as soon as they change (Screen.luaSig).
    static func walkKey(_ k: String) -> Bool { k == "carousel" || k.hasPrefix("pages.") }
    /// What the owner's overrides of 2026-09-29 left on the twin until they were dropped (2026-09-30 19:35):
    /// the trains and flights pages off, the flight board on ZZZZ. While the twin's base has no such key - it
    /// was never compared since - and the twin still holds that value, it is the override's, not a person's:
    /// the panel's value goes to the twin, whichever way sync aligns.
    static func residue(_ k: String, twin v: Any?) -> Bool {
        switch k {
        case "pages.trains", "pages.flights": return (v as? NSNumber)?.boolValue == false
        case "flightboard.selection": return ((v as? [Any])?.first as? String) == noAsk.icao
        default: return false
        }
    }
    static let metricFields: [(String, String)] = [("metricLabels", "label_"), ("metricOrder", "order_"), ("metricCompanions", "companion_"),
        ("metricPositions", "position_"), ("metricBarPositions", "barPosition_"), ("metricBarMin", "barMin_"), ("metricBarMax", "barMax_"),
        ("metricBarWidths", "barWidth_"), ("metricBarOffsets", "barOffset_")]   // web.cpp:2047-2120; config.h MAX_METRICS 20
    static let carouselKeys = ["enabled", "idleS", "slotS", "allStyles"]
    static let knobKeys = ["reverse", "lockoutMs", "debounceMs", "detent"]
    static let railKeys = ["rows", "switch_s", "level", "stale_s", "due_min", "clock_seconds", "row_color", "head_color", "due_color"]
    static let budgetKeys = ["floor_min", "day_cap", "month_cap"]
    /// The airport the override of 2026-09-29 added and selected on the twin (sync.py NO_ASK until 2026-09-30):
    /// never compared, and removed from the twin where it is left (enforceOverrides).
    static let noAsk = (icao: "ZZZZ", name: "NO REQUESTS")
    /// The panel's own hardware, not the owner's taste (the owner, 2026-09-30): from the panel to the twin
    /// only, never to the panel. Checked against the settings the firmware gives out (feat/sync-routes
    /// e3f6445: config.h:143-186, web.cpp handlePortalValues 1245-1293, handleSave 1925-2012,
    /// handleExportConfig 2516-2539, handleImportConfig 2758-2795; web_panel.cpp /api/knob):
    ///  - the microphones: where the sound comes from, the ES7210's gain and gate, the AGC
    ///    (config.h:182-186) - which the owner keeps off on the panel since 2026-09-15;
    ///  - the knob, all of /api/knob: lockout, debounce and detent calibrate its contacts, and reverse
    ///    stands in for its wiring ("instead of swapping A and B", control.h:24);
    ///  - how the presence radar is set up: presenceScaleM (the metres its fan covers in this room),
    ///    presenceMirrorX (which way +X points, as the radar is mounted), presenceSource (the radar, or
    ///    the scripted story where there is none) - config.h:154-159, only in the form (/save);
    ///  - the board's climate sensor: climateEnabled (read it), and its calibration - climateTempOffset,
    ///    climateHumOffset and climateRhFollowsT, the steps of climate::correct() (climate_model.h; the
    ///    measurement plan that finds them on this board, KB docs/21 §5.3-5.4) - config.h:143-150;
    ///  - irEnabled: listen to the remote's receiver (config.h:161-163, the form and /api/import).
    /// Not hardware, and mirrored: climateIntervalS (how often it is read, 5-300 s), climateShow (the
    /// weather screen's split), the remote's button functions (what a button does, by the owner's taste;
    /// its learned codes are never written at all).
    static let hardware: Set<String> = {
        let mine = ["audioSource", "micGainDb", "micGateDb", "micAgc", "presenceScaleM", "presenceMirrorX", "presenceSource",
                    "climateEnabled", "climateTempOffset", "climateHumOffset", "climateRhFollowsT", "irEnabled"]
        // Each under the name it has where it is read: "x." from the export, "f." from the form.
        return Set(mine.flatMap { ["x." + $0, "f." + $0] } + ["knob"])
    }()
    /// How a person reads them: "microphones, knob, presence radar, climate sensor, remote".
    static let hardwareWords = M("microphones, knob, presence radar, climate sensor and its calibration, the remote's receiver",
                                 "микрофон, ручка, радар присутствия, датчик климата и его калибровка, приёмник пульта")

    static func pick(_ d: J?, _ keys: [String]) -> J { var o: J = [:]; for k in keys { if let v = d?[k] { o[k] = v } }; return o }
    /// A custom city as compared: coordinates to 4 places, since they come back through a float (sync.py city_key).
    static func cityKey(_ c: J) -> [Any] {
        func r(_ x: Any?) -> Double { (((x as? NSNumber)?.doubleValue ?? 0) * 10000).rounded() / 10000 }
        return [c.s("name") ?? "", r(c["lat"]), r(c["lon"]), c.s("tz") ?? ""]
    }

    /// {name: value} of everything mirrored, from the routes' documents.
    static func flat(_ d: [String: J]) -> J {
        var s: J = [:]
        let export = d["/api/export"] ?? [:]
        let form = d["/api/portal"]?.o("form") ?? [:]
        for (k, v) in export where !exportSkip.contains(k) { s["x." + k] = v }
        if let tz = export["timezoneString"] { s["tz"] = tz }
        for (k, v) in form where export[k] == nil && !formSkip.contains(k) { s["f." + k] = v }
        if let on = d["/api/info"]?["logOn"] { s["logOn"] = on }
        let pages = d["/api/panel"]?.a("pages") ?? []
        if let pn = d["/api/panel"] {
            for p in pn.a("pages") {
                guard let key = p.s("key"), !pageSkip.contains(key), s["pages." + key] == nil else { continue }
                s["pages." + key] = p["on"] ?? true
            }
            if let c = pn["cardsOn"] { s["pages.cards"] = c }
            if let car = pn.o("carousel") { s["carousel"] = pick(car, carouselKeys) }
        }
        if let kn = d["/api/knob"] { s["knob"] = pick(kn, knobKeys) }
        if let wc = d["/api/worldclock"] {
            let cities = wc.a("cities").sorted { ($0.i("id") ?? 0) < ($1.i("id") ?? 0) }
            s["worldclock.cities"] = cities.filter { $0.s("kind") == "custom" }.map(cityKey)
            let home = cities.first { $0.i("id") == wc.i("home") }
            if wc.b("homeChosen") == true, let n = home?.s("name") { s["worldclock.home"] = n } else { s["worldclock.home"] = NSNull() }
        }
        if let rb = d["/api/railboard"] {
            s["railboard.crs"] = rb["crs"] ?? NSNull()
            s["railboard.favourites"] = rb["favourites"] ?? [Any]()
            let cfg = rb.o("cfg") ?? [:]
            // "web": the portal set them and NVS holds them; else Home Assistant's or the build's (railboard.cpp:1103).
            s["railboard.config"] = cfg.s("from") == "web" ? pick(cfg, railKeys) as Any : "ha-or-build" as Any
        }
        if let fb = d["/api/flightboard"] {
            // The airport selected, by its ICAO code (an id is a slot on this device), and which half it shows.
            let sel = fb.a("airports").first { $0.i("id") == fb.i("airport") }
            s["flightboard.selection"] = [sel?.s("code") ?? NSNull(), fb.s("dir") ?? NSNull()] as [Any]
            s["flightboard.custom"] = fb.a("airports").filter { $0.s("kind") == "custom" && $0.s("code") != noAsk.icao }
                .map { [$0.s("code") ?? "", $0.s("iata") ?? "", $0.s("name") ?? "", $0.s("tz") ?? ""] }
                .sorted { canon($0) < canon($1) }
            let direct = fb.o("direct") ?? [:]
            if direct.b("built") != false, let bud = direct.o("budget") { s["flightboard.budget"] = pick(bud, budgetKeys) }
            s["flightboard.tracked"] = fb.a("tracked").compactMap { $0.s("ident") }.sorted()
        }
        if let cfg = d["/api/market"]?.o("config") { for (k, v) in cfg { s["market." + k] = v } }
        if let md = d["/api/media"] { s["media.selected"] = md.s("selected") ?? "" }
        // A "page" button's page is an index into this device's pages (panelShowPage(arg),
        // ir_actions.cpp:132-133), and indexes differ per device: it is compared by key and name.
        for bt in d["/api/ir"]?.a("buttons") ?? [] {
            guard let n = bt.i("n") else { continue }
            let fn = bt.s("fn") ?? "none"
            var arg: Any = NSNull()
            if fn == "page", let i = bt.i("page") {
                if let p = pages.first(where: { $0.i("i") == i }) { arg = "\(p.s("key") ?? "")/\(p.s("name") ?? "")" } else { arg = "#\(i)" }
            }
            s["ir.\(n).fn"] = [fn, arg] as [Any]
        }
        return s
    }

    /// Which routes a key's value comes from: what is read again after writing it.
    static func routes(of key: String) -> [String] {
        if key.hasPrefix("x.") || key.hasPrefix("f.") || key == "tz" { return ["/api/portal", "/api/export"] }
        if key.hasPrefix("pages.") || key == "carousel" { return ["/api/panel"] }
        if key == "knob" { return ["/api/knob"] }
        if key == "logOn" { return ["/api/info"] }
        if key.hasPrefix("ir.") { return ["/api/ir"] }
        let head = key.split(separator: ".").first.map(String.init) ?? key
        return ["/api/" + head]
    }
    /// A key as a person reads it: "x.dimBrightness" -> "dimBrightness".
    static func shown(_ k: String) -> String { k.hasPrefix("x.") || k.hasPrefix("f.") ? String(k.dropFirst(2)) : k }
}

// MARK: - panels on the network (mDNS)

/// The firmware announces _http._tcp with TXT version, model and mac (network.cpp:263-266).
final class PanelFinder: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    struct Found: Equatable { let service: String, name: String, address: String, mac: String, version: String }
    private var browser = NetServiceBrowser()
    private var resolving: [NetService] = []
    private(set) var found: [String: Found] = [:]
    private(set) var started = false
    var onChange: (([Found]) -> Void)?

    func start() {
        guard !started else { return }
        started = true
        browser.delegate = self
        browser.searchForServices(ofType: "_http._tcp.", inDomain: "local.")
    }
    /// Everything looked up again: a panel back after a power cut, or a Mac back on its network, may
    /// have another address, and a service that left without a goodbye is still listed (main thread).
    func refresh() {
        guard started else { return }
        browser.stop(); for s in resolving { s.stop() }
        resolving = []; found = [:]; onChange?([])
        browser = NetServiceBrowser(); started = false
        start()
    }
    func netServiceBrowser(_ b: NetServiceBrowser, didFind s: NetService, moreComing: Bool) {
        resolving.append(s); s.delegate = self; s.resolve(withTimeout: 8)
    }
    func netServiceBrowser(_ b: NetServiceBrowser, didRemove s: NetService, moreComing: Bool) {
        found[s.name] = nil; resolving.removeAll { $0 == s }; onChange?(Array(found.values))
    }
    func netServiceDidResolveAddress(_ s: NetService) {
        defer { resolving.removeAll { $0 == s } }
        let txt = s.txtRecordData().map { NetService.dictionary(fromTXTRecord: $0) } ?? [:]
        func t(_ k: String) -> String { txt[k].map { String(decoding: $0, as: UTF8.self) } ?? "" }
        guard t("model") == "AnimatedPixelClock", let ip = PanelFinder.ipv4(s.addresses ?? []) else { return }
        let addr = s.port == 80 || s.port <= 0 ? ip : "\(ip):\(s.port)"
        found[s.name] = Found(service: s.name, name: (s.hostName ?? s.name).replacingOccurrences(of: ".local.", with: ""),
                              address: addr, mac: normMac(t("mac")), version: t("version"))
        onChange?(Array(found.values))
    }
    func netService(_ s: NetService, didNotResolve e: [String: NSNumber]) { resolving.removeAll { $0 == s } }

    static func ipv4(_ addrs: [Data]) -> String? {
        for d in addrs {
            let ip: String? = d.withUnsafeBytes { raw in
                guard raw.count >= MemoryLayout<sockaddr_in>.size else { return nil }
                let sa = raw.load(as: sockaddr_in.self)
                guard sa.sin_family == sa_family_t(AF_INET) else { return nil }
                var a = sa.sin_addr; var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
                return inet_ntop(AF_INET, &a, &buf, socklen_t(INET_ADDRSTRLEN)).map { _ in String(cString: buf) }
            }
            if let ip { return ip }
        }
        return nil
    }
}

// MARK: - the engine

final class SyncEngine {
    enum Direction { case fromPanel, fromTwin }
    /// At a resume, when the twin was changed while sync was off: its changes go to the panel, or the
    /// twin takes the panel's state back.
    enum ResumeChoice { case toPanel, fromPanel }
    enum Reply { case consent(Bool), direction(Direction), resume(ResumeChoice), cancel }
    /// The first window of switching sync on: "Enable sync with the panel <name, address, MAC>?".
    /// pressForTesting: 1 presses Return in it, 2 presses Enable and then Return in the second window.
    struct Consent { var id = "", panelName = "", panelAddress = "", panelMac = "", twin = "", pressForTesting = 0 }
    /// The second window: the alignment, with what differs. pressReturnForTesting presses Return in it.
    /// notCarried: the effects a side refused to take ("AUTUMN DAWN to the twin: too slow…"), left out of the alignment;
    /// renames: the twin's files named as the panel's but for case ("flow → FLOW"), which take the panel's names.
    struct Summary {
        var id = "", sig = "", panel = "", twin = "", panelFirmware = "", twinFirmware = "", settings: [String] = [], hardware: [String] = []
        var onlyPanel: [String] = [], onlyTwin: [String] = [], differ: [String] = [], effectsKnown = true, bySize = false
        var notCarried: [Msg] = [], renames: [String] = []
        var panelScreen = "", twinScreen = "", why: Msg?, pressReturnForTesting = false
    }
    /// The second window when sync resumes with a pair it knows: what each side changed while it was off.
    struct ResumeSummary { var id = "", panel = "", twin = "", settings: [String] = [], effects: [Msg] = [], conflicts: [String] = [],
                           hardware: [String] = [], panelSettings: [String] = [], panelEffects: [Msg] = [], notCarried: [Msg] = [],
                           pressReturnForTesting = false }
    /// "Update the panel too?" and "Really flash the physical panel?": what the yes is for - this panel
    /// (its MAC), this firmware on it, the twin's firmware - kept with the question, checked again when it
    /// is answered and once more right before the image is sent. pressForTesting: 1 presses Return in the
    /// first window, 2 presses Update in it and then Return in the second.
    struct Offer {
        let id: String, pair: String, panelName: String, panelAddress: String, panelMac: String
        let panelVersion: String, panelBuild: String, panelId: String, twinId: String, version: String, build: String
        let source: Msg, downgrade: Bool, pressForTesting: Int
    }
    struct Status { var phase = M("off", "выключена"); var peer = ""; var last: (Date, Msg)?; var problem: (Date, Msg)?; var sticky = false }

    // Thresholds of our choosing, not measured: a change bigger than this on either side waits for the person.
    static let MASS_SETTINGS = 12, MASS_DELETES = 2
    static let fastEvery: TimeInterval = 3, effectsEvery: TimeInterval = 60, verifyEvery: TimeInterval = 15
    // Under strain (webRefused or allocFails grew): the portal's own cadence, and more than
    // NET_TURN_QUIET_MS = 1500 between requests (net_turns.h:32, 47); for 10 min, our choice.
    static let strainedFastEvery: TimeInterval = 5, strainedPace: TimeInterval = 1.6, strainFor: TimeInterval = 600
    // A script's SHA-256 (GET /api/lua/source) is read only to see an edit that keeps its length - a size that changed
    // or a script new to the list is seen by GET /api/lua - one script of a side at a time, never two in a row: at
    // least hashGap between two reads of a side, and each script read again about once in hashCircle (the slot,
    // hashCircle over its scripts, hashGap at least). A script with no SHA-256 for its size goes first. Our choice:
    // 35 reads in a row every 5 min held the panel's loop() up to 3.8 s each and a person's change waited 15 s, and its
    // free heap fell 38184 -> 28128 B; the twin's 35 every minute (1.2 MB) loaded the engine (the owner's pair, app 1.4,
    // 2026-10-01 05:44-06:06).
    // A read cut off for a person's change is made again this long after at the soonest. Our choice.
    static let hashGap: TimeInterval = 8, hashCircle: TimeInterval = 330, hashRetry: TimeInterval = 3
    static let fwWatchMax: TimeInterval = 300, confirmWithin: TimeInterval = 300
    // A failed transfer to the twin: tried again after these pauses, and after CARRY_TRIES failures in a row
    // not until "Sync now" - each one may read the whole image out of the panel. Our choice.
    static let carryBackoff: [TimeInterval] = [60, 300, 900], CARRY_TRIES = 4
    // Effects that cannot be known now (the /api/lua/source probe got no answer): a question waits for them
    // this long, reading them again every fxRetry. Our choice.
    static let fxWaitMax: TimeInterval = 60, fxRetry: TimeInterval = 10
    // The leading panel's carousel step (stepRead): the rounds that take a while wait while it is this near,
    // this long at most; a read that looks for it and does not see it looks again, this many times at most;
    // it looks this long after the latest moment the step can come. The twin's carousel is held again when its
    // hold would end within holdAhead, before the panel's (twinMove). Our choice.
    static let stepQuiet: TimeInterval = 3.5, stepQuietMax: TimeInterval = 30, STEP_TRIES = 3, stepMargin: TimeInterval = 0.08
    static let holdAhead: TimeInterval = 8
    // A write to the twin is kept once its old boot answered this long after it: a module saves its settings to
    // NVS 2.5 s after a change (SETTLE_MS: panel.cpp:24, clock_style.cpp:25, railboard.cpp:418, media_ha.cpp:87,
    // MARKET_NVS_SETTLE_MS market.h:61; firmware 2.7.9), and 1.5 s more for the two requests' way and a pass of
    // loop(). Our choice.
    static let lostWithin: TimeInterval = 4
    /// sync.log is started again past this size, the old one kept as sync.log.1. Our choice.
    static let logKeep = 4 << 20

    let dataDir: URL
    var logFile: URL { dataDir.appendingPathComponent("sync.log") }
    var stateFile: URL { dataDir.appendingPathComponent("sync-state.json") }

    // Shared with the main thread, under the lock.
    private let lock = NSLock()
    private var _enabled = false, _twinAddress: String?, _twinMac = "", _found: [PanelFinder.Found] = []
    private var _lang = "en", _syncNow = false, _reset = false
    /// The switch was on when the app started and has not been switched off since (markLaunch, setEnabled): only
    /// then does a saved consent count (keptConsent). A switch-off forgets the consent (_dropConsent); quitting
    /// the app does not (quit).
    private var _launchOn = false, _dropConsent = false, _quitting = false
    /// The instant events: the datagrams the listener's thread got, waiting for the worker; when the twin's page or its
    /// console told of a person there (wakeFromTwin), the round it asks for; the twin's UDP port forwarded by the
    /// engine's NAT (nil on the home network); where each side is subscribed (quit() ends it from the main thread).
    private var _datagrams: [(data: Data, ip: String, port: UInt16, at: Date)] = [], _wakeAt: Date?, _twinUdp: Int?
    private var _subs: [Side: (address: String, port: Int)] = [:]
    private var _reply: (id: String, reply: Reply)?, _fwReply: (id: String, go: Bool)?
    private var _status = Status()
    /// Called on the main thread when the status changed.
    var onStatus: (() -> Void)?
    /// The main thread asks the person; the answers come back through answerConsent / answerDirection /
    /// answerResume / answerFirmware.
    var askConsent: ((Consent) -> Void)?
    var askDirection: ((Summary) -> Void)?
    var askResume: ((ResumeSummary) -> Void)?
    var askFirmware: ((Offer) -> Void)?
    /// The panel found over mDNS stopped answering: look for it again (main thread).
    var onPanelLost: (() -> Void)?

    private func locked<T>(_ f: () -> T) -> T { lock.lock(); defer { lock.unlock() }; return f() }
    var enabled: Bool { locked { _enabled } }
    var status: Status { locked { _status } }
    /// Writes go on only while the switch is on and no new pair is being looked for.
    var writesAllowed: Bool { locked { _enabled && !_reset } }
    /// The person's switch (the window, the menu, `defaults write`): switched off, it also forgets the consent kept
    /// in sync-state.json - switching it on again asks both questions.
    func setEnabled(_ on: Bool) { locked { _enabled = on; if !on { _reset = true; _launchOn = false; _dropConsent = true } } }
    /// Before start(): whether the switch is on as the app starts (a saved consent counts only then). Off, the consent
    /// in the file is forgotten at once: it was switched off while the app was closed (`defaults write`).
    func markLaunch(on: Bool) { locked { _launchOn = on; _dropConsent = !on } }
    /// The app quits: nothing more is written, as when switched off, but the consent stays for the next start.
    func quit() {
        let subs = locked { () -> [Side: (address: String, port: Int)] in _enabled = false; _reset = true; _quitting = true; let s = _subs; _subs = [:]; return s }
        // The devices stop sending to this app now, not when the subscription lapses (60 s): the worker may not
        // run again before the app is gone.
        for (side, sub) in subs { SyncEngine.unlisten(Device(side, sub.address), port: sub.port) }
        wakeSem.signal()
    }
    /// The twin: where its API answers, its MAC, and - without the home network - the host port its UDP port 4210 is
    /// forwarded from (main.swift Twin.start: --hostfwd udp:<udpPort>-4210), the only way its events reach this Mac.
    func setTwin(address: String?, mac: String, udpForward: Int? = nil) {
        locked { _twinAddress = address; _twinMac = normMac(mac); _twinUdp = udpForward }
    }
    /// A person at the twin's page in the app (main.swift, the page's message "twinInput": the knob turned or pressed,
    /// a remote's button, BOOT): the twin is read a moment later - the firmware acts on the gesture first - unless the
    /// twin's own events (2.7.13) already tell of it. WHAT: "knob 1", "press", "ir", "boot"; "reset" asks for nothing (a
    /// restart is seen by its uptime, and a read now would only find the twin away).
    func twinPageInput(_ what: String) { if what != "reset" { wakeFromTwin(after: what.hasPrefix("knob") ? 0.06 : 0.15) } }
    /// A line of the twin's console (main.swift reads the engine's output): "[luafx] open <name>" - an effect opened
    /// there, by a person or by sync - asks for the screen round at once, under the same terms.
    func twinConsoleLine(_ line: String) { if line.contains("[luafx] open") { wakeFromTwin(after: 0.02) } }
    /// Tests only: false drops a datagram, as if it were lost on the way (the stand's loss test).
    var eventFilter: ((Side, J) -> Bool)?
    func setFound(_ f: [PanelFinder.Found]) { locked { _found = f } }
    func setLanguage(_ l: String) { locked { _lang = l } }
    func syncNow() { locked { _syncNow = true } }
    /// The panel's address or the repository changed: find the panel again.
    func reconnect() { locked { _reset = true } }
    func answerConsent(id: String, _ yes: Bool?) { locked { _reply = (id, yes.map { .consent($0) } ?? .cancel) } }
    func answerDirection(id: String, _ d: Direction?) { locked { _reply = (id, d.map { .direction($0) } ?? .cancel) } }
    func answerResume(id: String, _ c: ResumeChoice?) { locked { _reply = (id, c.map { .resume($0) } ?? .cancel) } }
    func answerFirmware(id: String, go: Bool) { locked { _fwReply = (id, go) } }

    // The worker's own.
    private enum QKind { case consent, direction, resume }
    private struct Question { let id: String, kind: QKind, sig: String }
    private struct Caps { var id = ""; var lua: Bool?; var image: Bool?; var elf: String? }
    /// A firmware sent by sync and not confirmed yet (kept in sync-state.json).
    private struct InFlight: Codable { var to: Side, from: Side, version: String, bytes: Int, build: String?, fromId: String, oldId: String, sent: Date }
    private var thread: Thread?
    private var dev: [Side: Device] = [:]
    private var pairKey = ""
    /// consented: the first window was answered Enable for this pair since the switch went on - or its answer was
    /// kept from the run before (consentKept, keptConsent); aligned: the second one too, and the sides were
    /// aligned. Nothing is written before both.
    private var consented = false, aligned = false, resumed = false, needDirection = false, consentKept = false
    private var question: Question?
    /// An answer that waits for the effects of both sides to be known before it is checked and done.
    private var heldReply: (id: String, reply: Reply)?
    private var alignRetry = Date.distantPast, fxWaitSince: Date?
    /// The last readAll read every script's content (settle): a question asked now lists them as they are.
    private var fxFresh = false
    /// A side whose effects cannot be read at all with its firmware (no Lua, no script store, no filesystem):
    /// not waited for. The rest of "unknown" is the /api/lua/source probe without an answer, which is.
    private var fxAbsent: [Side: Bool] = [:]
    /// The effects were not aligned with the rest (a side's could not be read): the direction the person
    /// chose, done once both sides' can be read (effectsPass).
    private var fxAlignFrom: Side?
    /// The image last read for a transfer, by the source's firmware id and ELF SHA-256.
    private var imageCache: (key: String, data: Data, fromRelease: Bool)?
    private var base: [Side: [String: [String: String]]] = [:]    // side -> "screen"|"settings"|"effects" -> field -> value
    private var firmwareBase: [Side: String] = [:]
    /// The screens as last read; when with /api/status too (screenAt: a reboot is a smaller uptime), and when
    /// /api/panel alone (pageReadAt: a step read, the answer to a write).
    private var screen: [Side: Screen] = [:], screenAt: [Side: Date] = [:], pageReadAt: [Side: Date] = [:]
    /// When the leading panel's carousel steps next, as a window of time (lo, hi], narrowed by each read of
    /// its /api/panel while it shows the same page and style (KEY); the read that looks for the step (stepAt),
    /// and how many looked in vain for this step.
    private var stepWin: (lo: Date, hi: Date, key: String)?, stepAt: Date?, stepTries = 0
    /// Screen fields sync wrote to a side and has not read back from it yet: a write whose answer was lost -
    /// the Panel group's 503 comes after the work was done (failOom, web_panel.cpp:145-160), a timeout - may
    /// still have landed. When the side shows that value later, it is sync's own write, not a person's change,
    /// and it does not go back (SyncEngine.screenChanges).
    private var wrote: [Side: [String: String]] = [:]
    private var effects: [Side: Effects] = [:]
    /// Effects a side refused to take (UploadRefused), by that side and effect: not sent to it again while the source
    /// holds the same content - and, refused for want of room, while the side has no fewer scripts (stillRefused).
    /// Said once when refused, and in the direction question ("not carried"); kept in sync-state.json.
    private var refusedFx: [Side: [String: Refusal]] = [:]
    /// Scripts a side lists and does not give out (NoSuchScript), with their size then: not asked for again until
    /// the size changes, the script leaves the list or the side runs another firmware.
    private var noSource: [Side: [String: Int]] = [:]
    /// The page a side shows after its effect list was renumbered under it (renumbered): nobody's choice.
    private var shifted: [Side: String] = [:]
    /// Renames the twin refused (renameTwinFiles), with the twin's content then: not tried again in this run while
    /// it holds that content.
    private var renameRefused: [String: String] = [:]
    private var docs: [Side: [String: J]] = [:]
    private var fw: [Side: Firmware] = [:]
    private var caps: [Side: Caps] = [:]
    /// Each side's scripts' SHA-256 (16 hex digits) by file name, with the size it was read for and when (kept in
    /// sync-state.json, without when); the last read of a side on the schedule (hashNext, hashTick).
    private var hashes: [Side: [String: (bytes: Int, hash: String, at: Date)]] = [:], hashLast: [Side: Date] = [:]
    /// The rounds that take a while (effects, settings, firmware, a script's SHA-256): the instant events are served
    /// before each of their requests (LongRounds.between, serveEvents).
    private let rounds = LongRounds()
    private var pendingFx: [Side: [String: Int]] = [:]              // effects that could not be carried yet
    private var noted = Set<String>()
    private var nextFast = Date.distantPast, nextSlow = Date.distantPast, nextEffects = Date.distantPast, nextVerify = Date.distantPast
    private var verifyNow = false
    private var fxDue = false, slowRounds = 0, fwWatch: Date?, fwWatchSince: [Side: Date] = [:]
    private var offer: Offer?, declined = Set<String>()
    private var wantPanel: String?, wantTwin: String?, inFlight: InFlight?
    private var carry = (tries: 0, next: Date.distantPast)
    private var gallery: (at: Date, repo: String, list: [J])?
    private var stateDirty = false, lastSaved: Data?
    private var down = Set<Side>()
    private var failures: [String: Int] = [:]
    private var load: (uptime: Int, refused: Int, fails: Int)?, strainedUntil: Date?
    private var panelLostAt = Date.distantPast
    private var holdWhy: Msg?                                          // why the direction is asked again (a massive change)
    private var stopLogged = false
    private var quietSince: Date?
    /// A side restarted: the carousel's settings wait for the settings round (fastRound).
    private var walkHeld = false
    /// Each side's uptime as last read - from /api/status or /api/info, whichever read it: one clock, millis()
    /// / 1000 (web.cpp:495, 632) - and when. A smaller one is a restart, whoever read it (sawUptime).
    private var uptimeSeen: [Side: (up: Int, at: Date)] = [:]
    /// A restart a read saw and that is not dealt with yet (restarts), with the last moment the side answered
    /// before it.
    private var restartSeen: [Side: Date] = [:]
    /// The screen round goes first, before the effects and settings rounds: after a device did not answer,
    /// after a round failed, after a restart - so nothing is compared before both screens are read again.
    private var needScreenRound = false
    /// The settings sync wrote to the twin, each with the twin's value before the write (a digest) and when:
    /// until a read shows its old boot answered lostWithin after the write (a module saves to NVS SETTLE_MS
    /// after a change), a restart may have lost it. The twin restarted: the writes it may have lost (twinLost),
    /// for the first full settings pass - a key whose value is again the one before the write is a lost write,
    /// and the panel's goes back; anything else that differs on the twin is its own change (settingsChanges).
    private var twinWrote: [String: (before: String, at: Date)] = [:], twinLost: [String: String]?
    /// A person at the twin (sawTwin): when a touch of its carousel was seen that sync did not make - cleared
    /// by sync's next write there; the twin's last hold read, and when; when sync last wrote to it (a page or a
    /// style holds its carousel, and so does a world clock's new home: web_panel.cpp:764-771).
    private var twinTouch: Date?, twinHoldSeen: (hold: Int, at: Date, on: Bool)?, twinWroteAt = Date.distantPast, personSaid = false

    // The instant events (firmware 2.7.13; "Instant events" in the header).
    /// Wakes the worker: a datagram came, the twin's page or console told of a person, the app quits.
    private let wakeSem = DispatchSemaphore(value: 0)
    private var listener: EventListener?, listenerFailed = false
    /// Each side's subscription: where its datagrams come from (HOST, and PORT behind the engine's NAT), the port it is
    /// asked to send to (SAY), since when this run of subscriptions holds (SINCE) and when it was renewed (RENEWED), the
    /// last datagram heard; the number of its last event (SEQ) and of the last gesture taken (INPUTSEQ: nil until the
    /// first read of /api/panel says where the firmware's numbers are - the gestures before it are history).
    struct Ev { var host = "", port: UInt16?, say = 0, since: Date?, renewed: Date?, heard: Date?, seq = 0, inputSeq: Int?, noRoute = Date.distantPast }
    private var ev: [Side: Ev] = [:]
    private var nextListen = Date.distantPast, lastWoken = Date.distantPast
    /// When sync last wrote to each side: a person's datagram within crossWindow of it may predate the write, and the
    /// screen round judges it instead (carryNow).
    private var lastWrite: [Side: Date] = [:]
    /// Inside a page ("Inside a screen" in the header): what waits to be done on a side, in order, each not before its
    /// time - an effect's click, a gesture on a page with a stop inside, the leaving of that stop.
    enum InsideAct: Equatable { case click(name: String, run: Int?), gesture(fn: String, key: String, name: String), leave(key: String, name: String) }
    private var insideQ: [(to: Side, act: InsideAct, at: Date, since: Date)] = []
    /// The clicks of each side's effect taken into account (accounted): its run on this device, the clicks it had
    /// then that are no person's to carry (sync's own, carried ones, or those before sync saw it) - fx.clicks above it
    /// are a person's. The session each side showed at the first read (its clicks then are history), a side whose
    /// base is to be taken from its next reading (a click whose answer was lost), and a reconcile that is due.
    private var fxBase: [Side: (name: String, run: Int, base: Int)] = [:], fxFirst: [Side: String] = [:], fxRebase = Set<Side>()
    private var clicksDue = false
    /// The page sync or a side's carousel put on that side, until a person's page there, or until the effect it opened is
    /// taken into account (reconcileClicks: its clicks then are nobody's to carry). The run of a side's effect that did
    /// not open within openWait: no click is sent to it, and it is not read for one, until its run or its open changes.
    private var movedTo: [Side: String] = [:], fxGaveUp: [Side: Int] = [:]
    /// A person's presses on each side, with when they were made: the clicks carried keep their spacing (a double
    /// press is a gesture of its own on KINETIC DIGITS and OCEANARIUM: 0.45 s, sync_events.h).
    private var presses: [Side: [Date]] = [:]
    /// The gestures the last read of /api/panel?input= gave, taken at the end of the screen round.
    private var ringInputs: [Side: [J]] = [:]
    /// A firmware without now.fx (before 2.7.13): the knob's clicks (/api/knob stats click + long) on the page shown,
    /// as last read - the bridge of the design (w4kseii7h, plan step 2).
    private var knobSeen: [Side: (page: String, n: Int)] = [:]
    /// What a side's stop inside a page is expected to be after the gestures sync sent it, until its own events or a
    /// read say.
    private var enteredExpect: [Side: (value: Bool, until: Date)] = [:]
    /// When sync last did something inside a page on each side; the sides a datagram moved that the screen round has
    /// not read since (nothing is clicked there until it has).
    private var insideDone: [Side: Date] = [:], stale = Set<Side>(), serving = false

    init(dataDir: URL) {
        self.dataDir = dataDir
        rounds.serve = { [weak self] in try self?.serveEvents() }
    }

    func start() {
        guard thread == nil else { return }
        let t = Thread { [weak self] in
            while let self {
                self.step()
                _ = self.wakeSem.wait(timeout: .now() + self.pause)
            }
        }
        t.name = "twin-sync"; t.qualityOfService = .utility
        thread = t; t.start()
    }

    // MARK: status and log

    private var lang: String { locked { _lang } }
    private func T(_ m: Msg) -> String { m.text(lang) }

    private func publish(_ change: (inout Status) -> Void) {
        locked { change(&_status) }
        DispatchQueue.main.async { self.onStatus?() }
    }
    private func phase(_ m: Msg) { if locked({ _status.phase.en != m.en }) { publish { $0.phase = m } } }

    /// One line in <dataDir>/sync.log and the status: "2026-09-30 12:00:01.234 panel→twin screen page WARP".
    private let logLock = NSLock()
    private func log(_ dir: String, _ what: String, _ m: Msg, problem: Bool = false, sticky: Bool = false) {
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        let line = "\(f.string(from: Date())) \(dir) \(what) \(T(m))\n"
        logLock.lock(); defer { logLock.unlock() }                     // the worker, and the app's own lines (note)
        // A walking carousel writes a line a slot: past logKeep bytes the log becomes sync.log.1 (the one
        // before is dropped) and a new one starts.
        if let size = (try? FileManager.default.attributesOfItem(atPath: logFile.path))?[.size] as? Int, size > SyncEngine.logKeep {
            let old = dataDir.appendingPathComponent("sync.log.1")
            try? FileManager.default.removeItem(at: old); try? FileManager.default.moveItem(at: logFile, to: old)
        }
        if let h = try? FileHandle(forWritingTo: logFile) { h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close() }
        else { try? line.write(to: logFile, atomically: false, encoding: .utf8) }
        publish { if problem { $0.problem = (Date(), m); $0.sticky = sticky } else { $0.last = (Date(), m); $0.problem = nil } }
    }
    /// A line of the app's own in sync.log (main.swift: the owner's override at the twin's start, holdAskHa), from any thread.
    func note(_ what: String, _ m: Msg) { log("note", what, m) }
    /// A standing condition: logged once (until sync starts again).
    private func noteOnce(_ key: String, _ m: Msg) { if noted.insert(key).inserted { log("note", "-", m, problem: true, sticky: true) } }

    private func describe(_ e: Error) -> Msg {
        switch e {
        case let s as SyncError: return s.msg
        case let d as SyncDown: return d.msg
        case let w as SyncWait: return w.msg
        case is SyncStopped: return M("sync was switched off", "синхронизацию выключили")
        case let r as SyncRestart: return M("\(r.side.word.en) restarted", r.side == .panel ? "панель перезагрузилась" : "двойник перезагрузился")
        case let f as Failure: return M(f.description, f.description)
        default: return M(e.localizedDescription, e.localizedDescription)
        }
    }

    // MARK: the loop

    private func step() {
        let (on, reset, drop, quitting) = locked { () -> (Bool, Bool, Bool, Bool) in
            let r = _reset, d = _dropConsent; _reset = false; _dropConsent = false; return (_enabled, r, d, _quitting)
        }
        if reset || !on {
            if !dev.isEmpty || consented || aligned || question != nil || offer != nil {
                if !on && !quitting && (consented || aligned || question != nil) {
                    log("note", "off", aligned ? M("sync switched off: nothing more is written; switching it on asks both questions again",
                                                   "синхронизацию выключили: больше ничего не пишу; при включении снова задам оба вопроса")
                                               : M("sync switched off before both questions were answered: nothing was written",
                                                   "синхронизацию выключили до ответа на оба вопроса: ничего не записано"))
                }
                if drop { consented = false }                          // not kept in the state saved now
                unlistenAll()                                          // the devices stop sending at once
                forget()
            }
            if drop { dropSavedConsent() }                             // whatever the file holds: a switch-off forgets it
        }
        guard on else { phase(M("off", "выключена")); locked { _datagrams = []; _wakeAt = nil }; return }
        stopLogged = false
        do {
            try connect()
            try verifyIfDue()
            try restarts()                                             // a restart any read saw: first of all
            // The owner's two confirmations: until both are given, nothing is written.
            guard consented else { try consentFirst(); return }
            guard aligned else { try align(); return }
            if let (id, go) = locked({ () -> (String, Bool)? in let a = _fwReply; _fwReply = nil; return a }) { try answeredFirmware(id: id, go: go) }
            // The instant events: what came carried at once (a datagram may ask for the screen round now), then the
            // subscriptions renewed.
            try drainEvents()
            listenIfDue()
            let now = Date()
            let forced = locked { () -> Bool in let f = _syncNow; _syncNow = false; return f }
            if forced { declined = []; carry = (0, .distantPast); retryPending() }
            // The twin's page or console told of a person there, and its own events do not (takeWake): the twin is read now.
            let woke = takeWake(now)
            if now >= nextFast || forced {
                try fastRound(); nextFast = Date().addingTimeInterval(strained ? SyncEngine.strainedFastEvery : SyncEngine.fastEvery)
                needScreenRound = false
                planStep()
            } else if woke {
                try twinRound(); lastWoken = Date()
            } else if let at = stepAt, now >= at {
                // The panel's events bring its carousel's step themselves (followStep): its moment stays planned only
                // to keep the long rounds away from it.
                if live(.panel) { if now.timeIntervalSince(at) > 1.5 { stepAt = nil } } else { try stepRead() }
            }
            try runInside()
            guard aligned else { return }
            // The leader's next step is near: the rounds that take a while (effects, settings) wait until it
            // is carried, stepQuietMax at most - with a short slot a step is always near. After a failure, a
            // device that did not answer or a restart, they wait for the screen round (needScreenRound).
            let near = !forced && (stepAt.map { $0.timeIntervalSince(now) < SyncEngine.stepQuiet } ?? false)
            quietSince = near ? (quietSince ?? now) : nil
            let quiet = needScreenRound || (near && now.timeIntervalSince(quietSince ?? now) < SyncEngine.stepQuietMax)
            // The rounds that take a while serve the instant events between any two of their requests (long, between).
            if (fxDue || forced || now >= nextEffects) && !quiet {
                fxDue = false
                try long { try effectsRound() }; nextEffects = Date().addingTimeInterval(SyncEngine.effectsEvery)
            } else if !quiet {
                // A script's SHA-256 on its schedule: one read of one side at most (hashNext).
                try long { for s in sides { if try hashTick(s, effects[s]) { break } } }
            }
            guard aligned else { return }
            if (forced || now >= nextSlow) && !quiet {
                fwWatch = nil
                try long { try slowRound(market: forced || slowRounds % 5 == 0) }; slowRounds += 1
                nextSlow = Date().addingTimeInterval(settingsEvery)
            } else if let w = fwWatch, now >= w, !quiet {
                fwWatch = Date().addingTimeInterval(15)                  // a new image: its /api/info only, again if this fails
                try long { for s in sides { try readDocs(s, ["/api/info"]) } }
                fwWatch = nil
                firmwareRound()
            }
            guard aligned else { return }
            try long { try firmwareDue() }
            saveState()
            phase(M("on", "включена"))
            if locked({ _status.problem != nil && !_status.sticky }) { publish { $0.problem = nil } }
        } catch is SyncStopped {
            if !stopLogged { stopLogged = true; log("note", "stop", M("sync was switched off: writing stopped", "синхронизацию выключили: запись остановлена")) }
        } catch let w as SyncWait {
            phase(w.msg)
            Thread.sleep(forTimeInterval: 2)
        } catch is SyncRestart {
            // restartSeen has it: the next step deals with it before anything else (restarts), at once.
            stepAt = nil; needScreenRound = true
        } catch let d as SyncDown {
            if down.insert(d.side).inserted { log("error", "down", d.msg, problem: true) }
            verifyNow = true
            phase(d.msg)
            if d.side == .panel, (UserDefaults.standard.string(forKey: "panelAddress") ?? "").trimmingCharacters(in: .whitespaces).isEmpty,
               Date().timeIntervalSince(panelLostAt) > 30 {
                panelLostAt = Date()                                     // looked for again over mDNS, at most every 30 s
                DispatchQueue.main.async { self.onPanelLost?() }
            }
            backOff()
            stepWin = nil                                                // its carousel's clock is not known any more
        } catch {
            log("error", "-", describe(error), problem: true)
            backOff()
        }
    }

    /// After a failure: the next screen round in 5 s, and nothing before it - not the read that looks for the
    /// leader's step (it is planned again after a screen round that read the panel), not the effects or the
    /// settings round (needScreenRound).
    private func backOff() {
        nextFast = Date().addingTimeInterval(5); stepAt = nil; needScreenRound = true
        Thread.sleep(forTimeInterval: 3)
    }

    /// The panel under strain (its refused requests or failed allocations grew, watchLoad): read less.
    private var strained: Bool { strainedUntil.map { Date() < $0 } ?? false }

    /// How long the worker sleeps between two steps: until the next screen round, the read that looks for the
    /// leader's carousel step, the next thing to do inside a page or the round the twin's page asked for, 0.5 s at
    /// most - or until a datagram wakes it.
    private var pause: TimeInterval {
        guard aligned else { return 0.5 }
        var next = nextFast
        // With the panel's events the step's moment only keeps the long rounds away: looked at again when it lapses.
        if let s = stepAt { let t = live(.panel) ? s.addingTimeInterval(1.6) : s; if t < next { next = t } }
        if let q = insideQ.first?.at, q < next { next = q }
        if let w = locked({ _wakeAt }), w < next { next = w }
        return min(0.5, max(0.01, next.timeIntervalSinceNow))
    }

    private var settingsEvery: TimeInterval {
        let s = UserDefaults.standard.integer(forKey: "syncSettingsEveryS")
        return TimeInterval(s <= 0 ? 60 : max(15, s))
    }
    private var repo: String { GitHub.repo }

    private func forget() {
        saveState()
        dev = [:]; pairKey = ""; consented = false; consentKept = false; aligned = false; resumed = false; needDirection = false; question = nil; holdWhy = nil
        heldReply = nil; alignRetry = .distantPast; fxWaitSince = nil; fxAbsent = [:]; fxAlignFrom = nil; imageCache = nil
        refusedFx = [:]; noSource = [:]; shifted = [:]; renameRefused = [:]
        base = [:]; firmwareBase = [:]; screen = [:]; screenAt = [:]; pageReadAt = [:]; wrote = [:]; effects = [:]; docs = [:]; fw = [:]; caps = [:]; hashes = [:]; hashLast = [:]
        stepWin = nil; stepAt = nil; stepTries = 0; quietSince = nil; walkHeld = false; twinTouch = nil; twinHoldSeen = nil; twinWroteAt = .distantPast; personSaid = false
        uptimeSeen = [:]; restartSeen = [:]; needScreenRound = false; twinWrote = [:]; twinLost = nil
        forgetInside(); ev = [:]; nextListen = .distantPast
        locked { _subs = [:]; _datagrams = []; _wakeAt = nil }
        pendingFx = [:]; noted = []; offer = nil; declined = []; wantPanel = nil; wantTwin = nil; inFlight = nil
        carry = (0, .distantPast); fwWatch = nil; fwWatchSince = [:]; slowRounds = 0; down = []; failures = [:]; load = nil; strainedUntil = nil
        nextFast = .distantPast; nextSlow = .distantPast; nextEffects = .distantPast; nextVerify = .distantPast; verifyNow = false
        locked { _reply = nil; _fwReply = nil; _status.peer = ""; _status.problem = nil }
    }

    // MARK: who is who

    /// The test switches hold only when the "panel" has a twin's MAC and the person named it by a loopback
    /// address in panelAddress, and syncPanelMayBeTwinForTesting is on: a real panel has neither, whatever
    /// its name - which anyone on the network can change (/api/rename, /save, /api/import).
    private func testGate(mac: String, address: String) -> Bool {
        let u = UserDefaults.standard
        let manual = (u.string(forKey: "panelAddress") ?? "").trimmingCharacters(in: .whitespaces)
        return u.bool(forKey: "syncPanelMayBeTwinForTesting") && !manual.isEmpty && manual == address && isLoopback(manual) && twinMacLike(mac)
    }
    private var testing: Bool { guard let p = fw[.panel], let d = dev[.panel] else { return false }; return testGate(mac: p.mac, address: d.address) }
    private func testSwitch(_ key: String) -> Bool { testing && UserDefaults.standard.bool(forKey: key) }
    private func testInt(_ key: String) -> Int { testing ? UserDefaults.standard.integer(forKey: key) : 0 }
    private func testAlign() -> String? {
        guard testing, let a = UserDefaults.standard.string(forKey: "syncAlignForTesting"), a == "panel" || a == "twin" else { return nil }
        return a
    }

    private var panelMac: String { String(pairKey.split(separator: "|").first ?? "") }
    private var twinMac: String { String(pairKey.split(separator: "|").last ?? "") }

    private func connect() throws {
        let (tAddr, tMac, found) = locked { (_twinAddress, _twinMac, _found) }
        guard let tAddr, !tMac.isEmpty else { throw SyncWait(M("waiting for the twin", "жду двойника")) }
        if dev[.twin]?.address != tAddr {
            let d = Device(.twin, tAddr)
            guard let info = try? d.get("/api/info") else { throw SyncWait(M("waiting for the twin's firmware", "жду прошивку двойника")) }
            let f = Firmware(info)
            guard f.name.hasPrefix("TWIN-") else { throw SyncWait(M("waiting for the twin to be named TWIN-…", "жду, пока двойник получит имя TWIN-…")) }
            guard f.mac == tMac else { throw SyncError(M("\(tAddr) answers with MAC \(f.mac), not this twin's \(tMac)", "\(tAddr) отвечает с MAC \(f.mac), а у этого двойника \(tMac)")) }
            d.mayWrite = { [weak self] in self?.writesAllowed ?? false }
            d.willWrite = { [weak self] in self?.twinWroteAt = Date(); self?.lastWrite[.twin] = Date() }
            d.before = { [weak rounds] in try rounds?.between() }
            d.yields = { [weak self] in self?.yieldNow(from: .twin) ?? false }
            dev[.twin] = d; fw[.twin] = f
            // A twin at another address, of the same pair: a restart is one, as ever (a new pair starts afresh).
            if "\(fw[.panel]?.mac ?? "")|\(f.mac)" == pairKey { try sawUptime(.twin, info.i("uptime"), at: Date()) }
        }
        let pAddr = try choosePanel(found, twinMac: tMac)
        if dev[.panel]?.address != pAddr {
            let d = Device(.panel, pAddr)
            guard let info = try? d.get("/api/info") else {
                throw SyncWait(M("the panel at \(pAddr) does not answer", "панель по адресу \(pAddr) не отвечает"))
            }
            let f = Firmware(info)
            if let why = refusal(f, address: pAddr, twinMac: tMac, twinAddress: tAddr) {
                noteOnce("refused-\(pAddr)-\(f.mac)", M("refused: " + why.en, "отказ: " + why.ru))
                throw SyncWait(why)
            }
            d.mayWrite = { [weak self] in self?.writesAllowed ?? false }
            d.willWrite = { [weak self] in self?.lastWrite[.panel] = Date() }
            d.before = { [weak rounds] in try rounds?.between() }
            d.yields = { [weak self] in self?.yieldNow(from: .panel) ?? false }
            dev[.panel] = d; fw[.panel] = f
            publish { $0.peer = "\(f.name) (\(pAddr), \(f.mac))" }
            if "\(f.mac)|\(fw[.twin]!.mac)" == pairKey { try sawUptime(.panel, info.i("uptime"), at: Date()) }
        }
        let key = "\(fw[.panel]!.mac)|\(fw[.twin]!.mac)"
        if key != pairKey {
            base = [:]; firmwareBase = [:]; pendingFx = [:]; noted = []; resumed = false; needDirection = false; question = nil; wrote = [:]
            stepWin = nil; stepAt = nil; stepTries = 0; twinTouch = nil; twinHoldSeen = nil
            uptimeSeen = [:]; restartSeen = [:]; twinWrote = [:]; twinLost = nil
            wantPanel = nil; wantTwin = nil; inFlight = nil; heldReply = nil; fxAlignFrom = nil; imageCache = nil
            refusedFx = [:]; noSource = [:]; shifted = [:]; renameRefused = [:]; hashes = [:]; hashLast = [:]   // another panel's scripts
            forgetInside(); ev = [:]; nextListen = .distantPast; locked { _subs = [:]; _datagrams = [] }
            pairKey = key; consented = false; consentKept = false; aligned = false; loadState()
            nextVerify = Date().addingTimeInterval(SyncEngine.verifyEvery)
        }
    }

    /// Why the device at ADDRESS must not be synced with as the panel, or nil.
    private func refusal(_ f: Firmware, address: String, twinMac: String, twinAddress: String) -> Msg? {
        if f.model != "AnimatedPixelClock" { return M("\(address) is not an AnimatedPixelClock panel", "\(address) — не панель AnimatedPixelClock") }
        if f.mac == twinMac || address == twinAddress {
            return M("\(address) is this twin itself: a twin is not synced with itself", "\(address) — это сам двойник: синхронизировать двойника с самим собой нельзя")
        }
        if twinLike(name: f.name, mac: f.mac) && !testGate(mac: f.mac, address: address) {
            return M("\(f.name) (\(f.mac)) is a twin, not a panel: choose the panel in Sync → Which panel…",
                     "\(f.name) (\(f.mac)) — двойник, а не панель: выберите панель в меню «Синхронизация → Какая панель…»")
        }
        return nil
    }

    private func choosePanel(_ found: [PanelFinder.Found], twinMac: String) throws -> String {
        let d = UserDefaults.standard
        let manual = (d.string(forKey: "panelAddress") ?? "").trimmingCharacters(in: .whitespaces)
        if !manual.isEmpty {
            guard validAddress(manual) else { throw SyncWait(M("the panel's address \"\(manual)\" is not a host or an IP", "адрес панели «\(manual)» — не имя и не IP")) }
            return manual
        }
        // Found over mDNS: never a twin, test switches or not (they need an address typed in).
        let want = normMac(d.string(forKey: "panelMac"))
        let panels = found.filter { $0.mac != twinMac && !twinLike(name: $0.name, mac: $0.mac) }
        if !want.isEmpty {
            if let p = panels.first(where: { $0.mac == want }) { return p.address }
            throw SyncWait(M("looking for the panel \(want) on the network", "ищу в сети панель \(want)"))
        }
        if panels.count == 1 { return panels[0].address }
        if panels.isEmpty { throw SyncWait(M("looking for the panel on the network (mDNS)", "ищу панель в сети (mDNS)")) }
        throw SyncWait(M("\(panels.count) panels on the network: choose one in Sync → Which panel…",
                         "в сети \(panels.count) панелей: выберите в меню «Синхронизация → Какая панель…»"))
    }

    /// The pair's MACs, read again: every 15 s, at once after a device answered again, and when an uptime
    /// jumped - before anything is written, since an address can change hands (DHCP, another panel, a twin).
    private func verifyIfDue() throws {
        guard verifyNow || !down.isEmpty || Date() >= nextVerify else { return }
        try verifyIdentity()
    }
    private func verifyIdentity() throws {
        for s in sides {
            guard let i = try device(s).get("/api/info") else { throw SyncError(M("\(s.word.en) has no /api/info", "\(s.at) нет /api/info")) }
            let f = Firmware(i), want = s == .panel ? panelMac : twinMac
            guard f.mac == want else {
                log("note", "who", M("\(device(s).address) now answers as \(f.name) (\(f.mac)), not \(want): nothing is written, connecting again",
                                     "по адресу \(device(s).address) теперь отвечает \(f.name) (\(f.mac)), а не \(want): ничего не пишу, подключаюсь заново"), problem: true)
                reconnect()
                throw SyncWait(M("the devices changed: connecting again", "устройства сменились: подключаюсь заново"))
            }
            fw[s] = f
            docs[s, default: [:]]["/api/info"] = i
            try sawUptime(s, i.i("uptime"), at: Date())
        }
        verifyNow = false; nextVerify = Date().addingTimeInterval(SyncEngine.verifyEvery)
        if !down.isEmpty { log("note", "back", M("both devices answer again", "оба устройства снова отвечают")); down = [] }
    }

    private func device(_ s: Side) -> Device { dev[s]! }

    // MARK: reading

    /// A side's screen: GET /api/panel and /api/status. The /api/panel document is kept with the settings'
    /// documents (docs): the carousel's settings and the pages it visits are compared from it every round.
    private func readScreen(_ s: Side) throws -> Screen {
        let d = device(s)
        let sent = Date()
        guard let pn = try d.panelDoc(input: screen[s]?.inputSeq != nil ? ev[s]?.inputSeq : nil) else {
            throw SyncError(M("\(s.word.en) has no /api/panel: its firmware is too old for sync", "\(s.at) нет /api/panel: прошивка слишком старая для синхронизации"))
        }
        let got = Date()
        guard let st = try d.get("/api/status") else {
            throw SyncError(M("\(s.word.en) has no /api/status: its firmware is too old for sync", "\(s.at) нет /api/status: прошивка слишком старая для синхронизации"))
        }
        // Restarted (a smaller uptime): nothing of this screen is anybody's change - the restart is dealt with
        // first (restarts), whichever round read it.
        try sawUptime(s, st.i("uptime"), at: Date())
        var x = Screen()
        x.apply(panel: pn)
        x.bright = st.i("brightness") ?? 0; x.off = st.b("forcedOff") ?? false; x.uptime = st.i("uptime") ?? 0
        docs[s, default: [:]]["/api/panel"] = pn; pageReadAt[s] = got
        if s == .panel { timeStep(x, sent: sent, received: got) } else { sawTwin(x, at: got) }
        noteInputs(s, x); stale.remove(s)
        return x
    }

    /// A side's uptime, from any read of it. Smaller than the one before: the side restarted since - noted
    /// (restartSeen, with the last moment it answered before), and the read throws SyncRestart, so nothing is
    /// decided on what it held before. The twin: which of sync's writes the restart may have lost (twinLost);
    /// a read of its old boot lostWithin after a write shows that write kept.
    private func sawUptime(_ s: Side, _ up: Int?, at: Date) throws {
        guard let up else { return }                                   // a firmware without the field: never a restart
        let prev = uptimeSeen[s]
        uptimeSeen[s] = (up, at)
        guard let prev, SyncEngine.restarted(before: prev.up, now: up) else {
            if s == .twin { twinWrote = twinWrote.filter { at.timeIntervalSince($0.value.at) < SyncEngine.lostWithin } }
            return
        }
        if restartSeen[s] == nil { restartSeen[s] = prev.at }
        if s == .twin {
            twinLost = (twinLost ?? [:]).merging(SyncEngine.mayBeLost(twinWrote, alive: prev.at)) { a, _ in a }
            twinWrote = [:]
        }
        throw SyncRestart(side: s)
    }
    /// One clock for both routes (millis() / 1000, web.cpp:495, 632): it only goes back when the device restarts.
    static func restarted(before: Int, now: Int) -> Bool { now < before }
    /// The writes to the twin a restart may have lost: those its old boot did not answer lostWithin after -
    /// ALIVE, the last moment it answered. Each with the twin's value before the write.
    static func mayBeLost(_ w: [String: (before: String, at: Date)], alive: Date) -> [String: String] {
        w.filter { alive.timeIntervalSince($0.value.at) < lostWithin }.mapValues { $0.before }
    }
    /// Of those, the ones the twin shows again as they were before the write (NOW: its digests): lost. Anything
    /// else that differs on it is its own change.
    static func lost(_ candidates: [String: String]?, now: [String: String]) -> Set<String> {
        Set((candidates ?? [:]).filter { now[$0.key] == $0.value }.keys)
    }

    /// The restarts reads saw (sawUptime), dealt with before anything else: the pair checked again, and each
    /// restarted side's screen taken as it is now - its reset screen, nobody's change (rebooted). The screen
    /// round comes next, then the settings round, which gives the twin back the writes its restart lost.
    private func restarts() throws {
        guard !restartSeen.isEmpty else { return }
        try verifyIdentity()
        for s in sides {
            guard restartSeen[s] != nil else { continue }
            if s == .twin { twinTouch = nil; twinHoldSeen = nil }       // its carousel's hold starts from boot: no touch
            // It forgot its listeners, and its numbers and effect runs start again: subscribed again at once.
            ev[s] = nil; nextListen = Date(); fxBase[s] = nil; fxFirst[s] = ""; knobSeen[s] = nil; enteredExpect[s] = nil
            fxGaveUp[s] = nil
            insideQ.removeAll { $0.to == s }
            let x = try readScreen(s); screenAt[s] = Date()
            restartSeen[s] = nil
            rebooted(s, x)
        }
        needScreenRound = true
    }
    /// A restart seen by a read whose failure was not thrown (a `try?`): the round stops before it decides anything.
    private func noRestartPending() throws { if let s = restartSeen.keys.first { throw SyncRestart(side: s) } }

    /// The uploaded scripts; nil when they cannot be known now - a build without Lua (404) or without the
    /// script store (no "uploaded"), a filesystem that did not mount (count 0 and fsFree 0:
    /// luaStoreFreeBytes, lua_store.cpp:126), or whether the scripts can be read is not known yet.
    /// Unknown is never taken for "no effects". Only GET /api/lua is read: each script's SHA-256 is the one read for the
    /// size it has now (hashes), and a script with none - new, or its size changed - is unknown (Effects.unknown): its
    /// read comes first on the schedule (hashNext), and it is compared once it is made. SETTLE: every script is read now,
    /// one after another - for a question, which lists what differs (readAll), and for effects aligned late.
    private func readEffects(_ s: Side, settle: Bool = false) throws -> Effects? {
        // Absent for good with this firmware, and not waited for; only the probe below can be "not now".
        guard let lua = try device(s).get("/api/lua"), let up = lua.o("uploaded") else { fxAbsent[s] = true; return nil }
        let scripts = (up["scripts"] as? [J]) ?? []
        if scripts.isEmpty && (up.i("count") ?? 0) == 0 && (up.i("fsFree") ?? 0) == 0 { fxAbsent[s] = true; return nil }
        fxAbsent[s] = false
        var e = Effects()
        e.names = lua["effects"] as? [String] ?? []
        let walk = (lua["inWalk"] as? [Any] ?? []).map { ($0 as? NSNumber)?.boolValue ?? true }
        for (i, n) in e.names.enumerated() { e.walk[n] = i < walk.count ? walk[i] : true }
        for sc in scripts {
            guard let stem = sc.s("name"), let b = sc.i("bytes") else { continue }
            e.bytes[shownName(stem)] = b; e.stem[shownName(stem)] = stem; e.files.append(stem)
        }
        noSource[s] = noSource[s]?.filter { e.stem.values.contains($0.key) }            // left the list: asked again if it comes back
        guard let byHash = luaCap(s) else { return nil }
        e.byHash = byHash
        guard byHash else { return e }
        for stem in e.files {
            let n = shownName(stem), b = e.bytes[n]!
            if settle && noSource[s]?[stem] != b { e.hash[n] = try scriptHash(s, stem: stem, bytes: b) }
            else if let c = hashes[s]?[stem], c.bytes == b { e.hash[n] = c.hash }
            else if noSource[s]?[stem] == b { continue }                            // not given out: by size until it changes
            else { e.unknown.insert(n) }
        }
        return e
    }

    /// Side S runs another firmware: what was read of its scripts stays known, and is read again first, one at a time on
    /// the schedule (not all at once, as before 1.4.1).
    private func ageHashes(_ s: Side) { hashes[s] = hashes[s]?.mapValues { ($0.bytes, $0.hash, Date.distantPast) } }

    /// The SHA-256s known now, put into E: a read made since E was read (hashTick) makes its effect known.
    private func knownNow(_ e: inout Effects, _ s: Side) {
        for n in e.unknown {
            guard let stem = e.stem[n], let c = hashes[s]?[stem], c.bytes == e.bytes[n] else { continue }
            e.hash[n] = c.hash; e.unknown.remove(n)
        }
    }

    /// A script's SHA-256 (16 hex digits), read now. A script the side lists and does not give out (NoSuchScript: the
    /// panel's LA_GIOCONDA, 30.09 21:23-22:58, a 404 in every round) is asked for once: then its last hash of that size,
    /// or none - compared by size - until its size changes. What is read is kept for the size it has, also when the list
    /// said another (then this throws: the list is read again). YIELDING: the read on the schedule - a person's change
    /// from either side cuts it off (Yielded): a big script's source holds the panel's loop() up to 3.8 s.
    private func scriptHash(_ s: Side, stem: String, bytes: Int, yielding: Bool = false) throws -> String? {
        let c = hashes[s]?[stem], old = c?.bytes == bytes ? c?.hash : nil
        if noSource[s]?[stem] == bytes { return old }
        let d: Data
        do { d = try device(s).luaSource(stem, yield: yielding ? { [weak self] in self?.yieldNow(from: nil) ?? false } : nil) }
        catch let e as NoSuchScript {
            noSource[s, default: [:]][stem] = bytes
            noteOnce("nosrc-\(s)-\(stem)-\(bytes)", M("\(s.word.en) lists the script \(stem) (\(bytes) B), but GET /api/lua/source?name=\(stem) answers 404 (\(e.why)): it is compared by size, and not asked for again until its size changes",
                                                       "\(s.word.ru) перечисляет скрипт \(stem) (\(bytes) Б), но GET /api/lua/source?name=\(stem) отвечает 404 («\(e.why)»): сравниваю его по размеру и не запрашиваю снова, пока размер не изменится"))
            return old
        }
        let h = String(sha256Hex(d).prefix(16))
        hashes[s, default: [:]][stem] = (d.count, h, Date())
        guard d.count == bytes else { throw SyncError(M("\(stem): \(d.count) bytes read, the list says \(bytes)", "\(stem): прочитано \(d.count) байт, в списке \(bytes)")) }
        return h
    }

    /// The slot of a side with N scripts read on the schedule: hashCircle over them, hashGap at least.
    static func hashSlot(_ n: Int) -> TimeInterval { max(hashGap, hashCircle / Double(max(n, 1))) }
    /// The next script's SHA-256 to read on a side, and from when: SCRIPTS its files in the device's order, with their
    /// sizes; CACHE the size each one was read for and when; NOSOURCE the ones it does not give out (with their size
    /// then: not read); LAST the side's last read. First a script with no SHA-256 for the size it has (new to the list,
    /// or its size changed) - the first such in the list - hashGap after LAST; else the one read longest ago, a slot
    /// (hashSlot) after LAST - so each is read again about once in hashCircle, one at a time. Nil: nothing to read.
    static func hashNext(scripts: [(stem: String, bytes: Int)], cache: [String: (bytes: Int, at: Date)], noSource: [String: Int],
                         last: Date) -> (stem: String, bytes: Int, at: Date)? {
        let readable = scripts.filter { noSource[$0.stem] != $0.bytes }
        if let f = readable.first(where: { cache[$0.stem]?.bytes != $0.bytes }) { return (f.stem, f.bytes, last.addingTimeInterval(hashGap)) }
        guard let o = readable.min(by: { cache[$0.stem]!.at < cache[$1.stem]!.at }) else { return nil }
        return (o.stem, o.bytes, last.addingTimeInterval(hashSlot(readable.count)))
    }

    /// The read due now on side S by its schedule (hashNext), one at most, of the scripts E lists: true when it was made.
    /// The effects round is asked for only when the content read is not what the side's base holds for that effect (an
    /// edit that kept the length, or a script new to the list) - a script read again as it was asks for nothing (GET
    /// /api/lua is 0.7 s on the panel). Inside a round that takes a while, the read gives way to a datagram
    /// A person's change cuts the read off (Yielded): it is made again hashRetry later. A list that changed meanwhile (the
    /// size read is not the list's): the effects round reads it again. INROUND: made by the effects round itself, which
    /// compares what it read at once.
    @discardableResult
    private func hashTick(_ s: Side, _ e: Effects?, inRound: Bool = false) throws -> Bool {
        guard let e, e.byHash else { return false }
        let scripts = e.files.compactMap { f in e.bytes[shownName(f)].map { (stem: f, bytes: $0) } }
        let cache = (hashes[s] ?? [:]).mapValues { (bytes: $0.bytes, at: $0.at) }
        guard let due = SyncEngine.hashNext(scripts: scripts, cache: cache, noSource: noSource[s] ?? [:], last: hashLast[s] ?? .distantPast),
              Date() >= due.at else { return false }
        hashLast[s] = Date()
        let h: String?
        do { h = try scriptHash(s, stem: due.stem, bytes: due.bytes, yielding: true) }
        catch is Yielded {
            // Cut off for a person's change: carried now. The read is made again hashRetry later at the soonest - after the
            // side's screen round (every 3 s) - never two of them in a row.
            hashLast[s] = Date().addingTimeInterval(SyncEngine.hashRetry - SyncEngine.hashGap)
            try serveEvents()
            return false
        } catch is SyncError { hashLast[s] = Date(); fxDue = true; return true }
        hashLast[s] = Date()                                             // the gap counts from the end of a read
        if let h, !inRound, base[s]?["effects"]?["s." + shownName(due.stem)] != "h" + h { fxDue = true }
        if var cur = effects[s] { knownNow(&cur, s); effects[s] = cur }
        return true
    }

    /// Whether a datagram waits that a read gives way to: a person's change or gesture (by knob, ir, http) or the leading
    /// carousel's step (by carousel) - from side SIDE, or from either (nil) - or the twin's page told of a person there.
    /// Not sync's own write coming back, not "auto" or the night's schedule. Asked from the worker only (ev).
    private func eventWaits(from side: Side?) -> Bool {
        let (got, woke) = locked { (_datagrams, _wakeAt.map { $0 <= Date() } ?? false) }
        if woke && side != .panel { return true }
        return got.contains { g in
            guard let o = (try? JSONSerialization.jsonObject(with: g.data)) as? J, let by = o.s("by"), SyncEngine.person(by) || by == "carousel" else { return false }
            guard let side, let e = ev[side] else { return side == nil }
            return e.since != nil && e.host == g.ip && (e.port == nil || e.port == g.port)
        }
    }

    /// The effects of side S as they are now; an error when they cannot be known.
    private func readEffectsNow(_ s: Side) throws -> Effects {
        guard let e = try readEffects(s) else { throw SyncError(M("the effects of \(s.word.en) cannot be read now", "эффекты \(s.of) сейчас не прочитать")) }
        return e
    }

    private static let settingsRoutes = ["/api/info", "/api/portal", "/api/export", "/api/panel", "/api/knob", "/api/worldclock",
                                         "/api/railboard", "/api/flightboard", "/api/media", "/api/ir"]

    private func readDocs(_ s: Side, _ routes: [String]) throws {
        for r in routes {
            let doc = try device(s).get(r)
            docs[s, default: [:]][r] = doc
            if r == "/api/info", let doc {
                fw[s] = Firmware(doc)
                try sawUptime(s, doc.i("uptime"), at: Date())               // restarted: the rest is not read now
            }
        }
    }

    /// A side's settings as compared. While the twin holds its flight board on ZZZZ (holdsNoAsk), the airport selected
    /// is compared on neither side - neither carried to the twin nor from it; once the hold ends, the twin's base has
    /// no such key and it still shows ZZZZ, so the panel's airport goes to it (Settings.residue).
    private func flat(_ s: Side) -> J {
        var f = Settings.flat(docs[s] ?? [:])
        if holdingNoAsk { f["flightboard.selection"] = nil }
        return f
    }
    private var holdingNoAsk: Bool { SyncEngine.holdsNoAsk(export: docs[.twin]?["/api/export"], board: docs[.twin]?["/api/flightboard"]) }
    private func digests(_ f: J) -> [String: String] { f.mapValues { digest($0) } }

    /// Everything of both sides. SETTLE, for a question (it lists what differs): every script's content read now (readEffects).
    /// Without it the scripts read before stand (kept in sync-state.json for a resume that asks nothing: the switch stayed
    /// on, and the twin runs only while the app does, so neither side's scripts were read meanwhile), and one whose size
    /// changed goes first on the schedule, as in any round.
    private func readAll(settle: Bool = false) throws {
        for s in sides {
            screen[s] = try readScreen(s); screenAt[s] = Date()
            try readDocs(s, SyncEngine.settingsRoutes + ["/api/market"])
            effects[s] = try readEffects(s, settle: settle)
        }
        fxFresh = settle
    }

    // MARK: the questions

    private func autoReply(_ id: String, _ r: Reply, _ key: String) {
        let delay = max(0, UserDefaults.standard.double(forKey: "syncAnswerDelayForTesting"))
        log("note", "ask", M("answered by itself in \(Int(delay)) s (\(key))", "ответ дам сам через \(Int(delay)) с (\(key))"))
        DispatchQueue.global().asyncAfter(deadline: .now() + delay) { [weak self] in self?.locked { self?._reply = (id, r) } }
    }

    /// The main thread's answer, taken once; an answer held back for the effects comes first.
    private func takeReply() -> (String, Reply)? {
        if let h = heldReply { heldReply = nil; return (h.id, h.reply) }
        return locked { () -> (String, Reply)? in let x = _reply; _reply = nil; return x.map { ($0.id, $0.reply) } }
    }

    private static let waitingForEffects = M("waiting until the effects of both sides can be read", "жду, когда станут видны эффекты обеих сторон")

    /// The first of the owner's two windows: "Enable sync with the panel <name, address, MAC>?". When it is
    /// asked, nothing has been read but who the two devices are (/api/info); nothing is written until it and
    /// the second window are answered. Cancel is its default button, and the main thread switches sync off.
    private func consentFirst() throws {
        if let (id, r) = takeReply() {
            guard let q = question, q.id == id, q.kind == .consent else { return }   // an answer to a question no longer open
            question = nil
            guard case .consent(true) = r else { return }                           // Cancel: the main thread switched sync off
            consented = true
            let p = fw[.panel]!
            log("note", "consent", M("sync with the panel \(p.name) (\(device(.panel).address), \(p.mac)) enabled by the person: the differences come next",
                                     "синхронизацию с панелью \(p.name) (\(device(.panel).address), \(p.mac)) включили: дальше — список различий и направление"))
            return
        }
        let waiting = SyncWait(M("waiting for your answer: enable sync with the panel?", "жду ответа: включать ли синхронизацию с панелью"))
        if question != nil { throw waiting }
        let p = fw[.panel]!, t = fw[.twin]!
        let c = Consent(id: UUID().uuidString, panelName: p.name, panelAddress: device(.panel).address, panelMac: p.mac,
                        twin: "\(t.name) (\(device(.twin).address), \(t.mac))", pressForTesting: testInt("syncEnableReturnForTesting"))
        question = Question(id: c.id, kind: .consent, sig: "")
        log("note", "ask", M("asking whether to enable sync with the panel \(p.name) (\(c.panelAddress), \(p.mac)); nothing is written until both questions are answered",
                             "спрашиваю, включать ли синхронизацию с панелью \(p.name) (\(c.panelAddress), \(p.mac)); пока нет ответа на оба вопроса, ничего не пишу"))
        if c.pressForTesting == 0 && testSwitch("syncConsentForTesting") { autoReply(c.id, .consent(true), "syncConsentForTesting") }
        else { DispatchQueue.main.async { self.askConsent?(c) } }
        throw waiting
    }

    private func askDirectionNow(_ why: Msg? = nil) {
        // The question lists what differs: every script's content read now, unless this very pass did (fxFresh).
        if !fxFresh {
            for s in sides where effects[s] != nil { if let e = try? readEffects(s, settle: true) { effects[s] = e } }
            fxFresh = true
        }
        var sum = summary(); sum.id = UUID().uuidString; sum.why = why
        sum.pressReturnForTesting = testInt("syncEnableReturnForTesting") == 2
        question = Question(id: sum.id, kind: .direction, sig: sum.sig)
        log("note", "ask", M("asking which way to align, with what differs", "спрашиваю, в какую сторону выровнять, со списком различий"))
        if !sum.pressReturnForTesting, let a = testAlign() { autoReply(sum.id, .direction(a == "panel" ? .fromPanel : .fromTwin), "syncAlignForTesting"); return }
        DispatchQueue.main.async { self.askDirection?(sum) }
    }

    private func effectWords(_ s: Side, _ keys: [String]) -> [Msg] {
        keys.map { k in
            let n = String(k.dropFirst(2)), e = effects[s]
            if k.hasPrefix("w.") { return e?.walk[n] ?? true ? M("\(n): into the walk", "\(n): в обход") : M("\(n): out of the walk", "\(n): из обхода") }
            if e?.bytes[n] == nil { return M("\(n): removed", "\(n): удалён") }
            return base[s]?["effects"]?[k] == nil ? M("\(n): new", "\(n): новый") : M("\(n): changed", "\(n): изменён")
        }
    }

    /// The second window when sync resumes with a pair it has saved bases for: what each side changed while
    /// it was off. Asked every time, also when the twin changed nothing - the second of the two confirmations.
    private func askResumeNow(_ plan: Plan) {
        var r = ResumeSummary(id: UUID().uuidString)
        r.panel = "\(fw[.panel]!.name) (\(device(.panel).address), \(fw[.panel]!.mac))"; r.twin = "\(fw[.twin]!.name) (\(device(.twin).address))"
        r.settings = plan.settings[.twin, default: []].map(Settings.shown)
        r.effects = effectWords(.twin, plan.effects[.twin, default: []])
        r.panelSettings = plan.settings[.panel, default: []].map(Settings.shown)
        r.panelEffects = effectWords(.panel, plan.effects[.panel, default: []])
        r.conflicts = plan.conflicts.map(Settings.shown); r.hardware = plan.hardware.map(Settings.shown)
        r.notCarried = notCarried()
        r.pressReturnForTesting = testInt("syncEnableReturnForTesting") == 2
        question = Question(id: r.id, kind: .resume, sig: plan.sig)
        log("note", "ask", M("sync resumes with a panel it knows: asking which way, with what each side changed while it was off",
                             "синхронизация продолжается с известной панелью: спрашиваю направление, со списком того, что каждая сторона меняла без неё"))
        if !r.pressReturnForTesting, let a = testAlign() { autoReply(r.id, .resume(a == "twin" ? .toPanel : .fromPanel), "syncAlignForTesting"); return }
        DispatchQueue.main.async { self.askResume?(r) }
    }

    // MARK: alignment

    /// Whether the effects of both sides are known, or cannot be with their firmware (fxAbsent). While a
    /// side's are only not known now - the /api/lua/source probe got no answer (a 503, the twin just
    /// started, the panel busy) - a question waits for them: fxRetry between reads, fxWaitMax at most.
    private func effectsReady() -> Bool {
        let unknown = sides.filter { effects[$0] == nil && fxAbsent[$0] == false }
        guard !unknown.isEmpty else { fxWaitSince = nil; return true }
        if fxWaitSince == nil {
            let who = unknown.map { $0.word.en }.joined(separator: " and "), кто = unknown.map { $0.of }.joined(separator: " и ")
            log("note", "effects", M("the effects of \(who) cannot be read just now: the question waits for them, \(Int(SyncEngine.fxWaitMax)) s at most",
                                     "эффекты \(кто) сейчас не прочитать: вопрос ждёт их, не дольше \(Int(SyncEngine.fxWaitMax)) с"))
        }
        let since = fxWaitSince ?? Date(); fxWaitSince = since
        if Date().timeIntervalSince(since) >= SyncEngine.fxWaitMax { return true }
        alignRetry = Date().addingTimeInterval(SyncEngine.fxRetry)
        return false
    }
    /// Effects not known just now, on either side (not the ones a firmware cannot give at all).
    private var fxUnknownNow: Bool { sides.contains { effects[$0] == nil && fxAbsent[$0] == false } }
    /// A side's effects base: what they were after the last alignment or round ("mode" is always in one).
    private func hasFxBase(_ s: Side) -> Bool { base[s]?["effects"]?["mode"] != nil }

    private func align() throws {
        if Date() < alignRetry { throw SyncWait(SyncEngine.waitingForEffects) }
        if let (id, r) = takeReply() {
            guard let q = question, q.id == id, q.kind != .consent else { return }  // an answer to a question no longer open
            if case .cancel = r { question = nil; return }                        // the main thread switched sync off
            do { try answered(q, r) }
            catch let e where !(e is SyncWait) {
                question = nil                                                   // asked again, with what holds then
                throw e
            }
            return
        }
        if question != nil { throw SyncWait(M("waiting for your answer", "жду ответа")) }
        // Waiting for the effects: they alone are read again, not the rest - then everything, for the question.
        // A resume with the consent kept asks nothing: the scripts read before stand (no batch of reads at a start); any
        // question reads them all (settle).
        let waited = fxWaitSince != nil, settle = !(resumed && !needDirection && consentKept)
        if waited { for s in sides where effects[s] == nil && fxAbsent[s] == false { effects[s] = try readEffects(s, settle: settle) } }
        else { try readAll(settle: settle) }
        guard effectsReady() else { throw SyncWait(SyncEngine.waitingForEffects) }
        if waited { try readAll(); fxFresh = settle }                // the side waited for read just now, the other before
        if resumed && !needDirection {
            if let why = resumeNeedsDirection() { needDirection = true; holdWhy = why; askDirectionNow(why) } else { try resume() }
        } else { askDirectionNow(holdWhy) }
        if !aligned { throw SyncWait(M("waiting for your answer", "жду ответа")) }
    }

    /// The answer, done for exactly what was shown: both sides are read again first - after the effects
    /// of both, if a side's are not known just now (the answer is held until they are, fxWaitMax at most) -
    /// and if anything the question listed changed meanwhile, it is asked again with the fresh list.
    private func answered(_ q: Question, _ r: Reply) throws {
        try readAll()                                                  // the scripts as the question read them a moment ago
        guard effectsReady() else { heldReply = (q.id, r); throw SyncWait(SyncEngine.waitingForEffects) }
        question = nil
        switch (q.kind, r) {
        case (.direction, .direction(let d)):
            let sum = summary()
            guard sum.sig == q.sig else {
                log("note", "ask", M("something the question listed changed while it was open: asking again", "пока вопрос был открыт, изменилось то, что в нём перечислено: спрашиваю снова"))
                askDirectionNow(M("Something changed while the question was open: this is the list now.", "Пока вопрос был открыт, что-то изменилось: вот список сейчас."))
                throw SyncWait(M("waiting for your answer", "жду ответа"))
            }
            try alignFrom(d == .fromPanel ? .panel : .twin)
        case (.resume, .resume(let c)):
            if let why = resumeNeedsDirection() { needDirection = true; holdWhy = why; askDirectionNow(why); throw SyncWait(why) }
            let plan = resumePlan()
            if let m = plan.mass { needDirection = true; holdWhy = m; log("note", "hold", m, problem: true); askDirectionNow(m); throw SyncWait(m) }
            guard plan.sig == q.sig else {
                log("note", "ask", M("something the question listed changed while it was open: asking again", "пока вопрос был открыт, изменилось то, что в нём перечислено: спрашиваю снова"))
                askResumeNow(plan); throw SyncWait(M("waiting for your answer", "жду ответа"))
            }
            switch c {
            case .toPanel: try carryResume()
            case .fromPanel: try alignFrom(.panel)
            }
        default: return
        }
    }

    /// Side FROM taken as it is: the other side gets its settings, effects and screen; the firmware is
    /// carried to the twin, or offered to the panel. Effects that cannot be read now on a side are aligned in
    /// this direction once both sides' can (effectsPass); where a side's firmware cannot give them at all,
    /// the direction is asked again the day it can.
    private func alignFrom(_ from: Side) throws {
        let to = from.other
        phase(M("first alignment: \(from.arrow)", "первое выравнивание: \(from.arrow)"))
        log(from.arrow, "align", M("alignment, \(from.word.en) as it is", "выравнивание: берётся \(from.word.ru) как есть"))
        // What the owner's old overrides left on the twin, never compared since: the panel's, whichever way.
        let twinNow = flat(.twin)
        let residue = settingsDiff(from: .panel).filter { Settings.residue($0, twin: twinNow[$0]) && base[.twin]?["settings"]?[$0] == nil }
        let keys = settingsDiff(from: from).filter { from == .panel || !residue.contains($0) }
        if !keys.isEmpty { try applySettings(from: from, keys: keys) }
        if from == .twin {
            // The panel's own hardware goes panel -> twin whichever way the rest is aligned (the owner, 2026-09-30).
            let hw = settingsDiff(from: .panel).filter { Settings.hardware.contains($0) || residue.contains($0) }
            if !hw.isEmpty { try applySettings(from: .panel, keys: hw) }
        }
        if to == .twin { try enforceOverrides() }
        var fxLater: Side?
        if let a = effects[from], let b = effects[to] { try convergeEffects(from: from, a, b); try renameTwinFiles() }
        else if fxUnknownNow {
            fxLater = from
            log("note", "effects", M("the effects of one side cannot be read now: they are aligned \(from.arrow) once both can be (more than \(SyncEngine.MASS_DELETES) removals ask again)",
                                     "эффекты одной из сторон сейчас не прочитать: выровняю их \(from.arrow), когда прочитаются обе (если удалять придётся больше \(SyncEngine.MASS_DELETES), спрошу снова)"))
        } else {
            log("note", "effects", M("the effects of one side cannot be read with its firmware: not aligned; the direction is asked the day both can be",
                                     "эффекты одной из сторон с её прошивкой не прочитать: не выравниваю; когда станут видны на обеих, спрошу направление"))
        }
        try readAll()
        try mirrorScreen(from: from, fields: SyncEngine.screenFields)
        try readAll()
        // The effects' bases only when both are known now; else none, so no round takes both as they are.
        let fxBoth = fxLater == nil && effects[.panel] != nil && effects[.twin] != nil
        if fxLater == nil && !fxBoth && fxUnknownNow { fxLater = from }             // converged, but not readable just now
        fxAlignFrom = fxLater
        for s in sides {
            rebaseScreen(s)
            base[s, default: [:]]["settings"] = digests(flat(s))
            base[s, default: [:]]["effects"] = fxBoth ? effects[s]!.fields : nil
            firmwareBase[s] = fw[s]!.id
        }
        // The firmware last: to the twin by itself (firmwareDue, with retries), to the panel after a question.
        if fw[from]!.id != fw[to]!.id {
            if from == .panel { wantTwin = fw[.panel]!.id; wantPanel = nil; carry = (0, .distantPast) }
            else { wantPanel = fw[.twin]!.id; wantTwin = nil }
        }
        aligned = true; resumed = true; needDirection = false; holdWhy = nil; stateDirty = true; fxWaitSince = nil
        if fw[.panel]!.mac != normMac(UserDefaults.standard.string(forKey: "panelMac")),
           (UserDefaults.standard.string(forKey: "panelAddress") ?? "").isEmpty {
            UserDefaults.standard.set(fw[.panel]!.mac, forKey: "panelMac")     // from now on this panel, by its MAC
        }
        log(from.arrow, "align", M("aligned: settings \(keys.count)", "выровнено: настроек \(keys.count)"))
        saveState()
    }

    /// Keys whose values differ, as side S would write them to the other side: never the panel's hardware
    /// to the panel.
    private func settingsDiff(from s: Side) -> [String] {
        let a = flat(s), b = flat(s.other)
        return a.keys.filter { b[$0] != nil && canon(a[$0]) != canon(b[$0]) && !(s == .twin && Settings.hardware.contains($0)) }.sorted()
    }

    private func summary() -> Summary {
        var m = Summary()
        m.panel = "\(fw[.panel]!.name) (\(device(.panel).address), \(fw[.panel]!.mac))"; m.twin = "\(fw[.twin]!.name) (\(device(.twin).address))"
        m.panelFirmware = fw[.panel]!.id; m.twinFirmware = fw[.twin]!.id
        let fp = flat(.panel), ft = flat(.twin)
        let diff = fp.keys.filter { ft[$0] != nil && canon(fp[$0]) != canon(ft[$0]) }.sorted()
        m.settings = diff.filter { !Settings.hardware.contains($0) }.map(Settings.shown)
        m.hardware = diff.filter { Settings.hardware.contains($0) }.map(Settings.shown)
        var sig = diff.map { "\($0)=\(digest(fp[$0]))/\(digest(ft[$0]))" }
        if let p = effects[.panel], let t = effects[.twin] {
            m.onlyPanel = p.bytes.keys.filter { t.bytes[$0] == nil }.sorted()
            m.onlyTwin = t.bytes.keys.filter { p.bytes[$0] == nil }.sorted()
            m.differ = p.bytes.keys.filter { t.bytes[$0] != nil && !Effects.same(p, t, $0) }.sorted()
            m.bySize = !(p.byHash && t.byHash)
            m.notCarried = notCarried()
            m.renames = SyncEngine.caseRenames(panel: p, twin: t).map { "\($0.from) → \($0.to)" }
            for s in sides { sig += effects[s]!.fields.map { "\(s).\($0.key)=\($0.value)" }.sorted() }
        } else { m.effectsKnown = false; sig.append("effects unknown") }
        sig.append("fw \(m.panelFirmware) / \(m.twinFirmware)")
        m.sig = sha256Hex(Data(sig.joined(separator: "\n").utf8))
        m.panelScreen = screen[.panel]!.name; m.twinScreen = screen[.twin]!.name
        return m
    }

    // MARK: resume

    /// What each side changed since the saved bases (sync-state.json), as the rounds would carry it.
    private struct Plan {
        var settings: [Side: [String]] = [:], effects: [Side: [String]] = [:], conflicts: [String] = [], hardware: [String] = []
        var mass: Msg?, sig = ""
    }

    /// Why a resume cannot go by the saved bases and the direction is asked instead: a side's effects are
    /// still not known after the wait (what either side changed in them cannot be shown), or the effects
    /// have no base (they were never aligned) - never merged by themselves either way. nil: it can.
    private func resumeNeedsDirection() -> Msg? {
        if fxUnknownNow {
            return M("The effects of one side still cannot be read, so what changed in them cannot be shown: which way? They are aligned in that direction once they can be read.",
                     "Эффекты одной из сторон всё ещё не прочитать, и что в них меняли, не показать: в какую сторону? Эффекты выровняю в эту сторону, когда они прочитаются.")
        }
        if effects[.panel] != nil && effects[.twin] != nil && !(hasFxBase(.panel) && hasFxBase(.twin)) {
            return M("The effects of the panel and the twin have not been aligned yet: which way?", "Эффекты панели и двойника ещё не выровнены: в какую сторону?")
        }
        return nil
    }

    private func resumePlan() -> Plan {
        var p = Plan()
        let cur: [Side: J] = [.panel: flat(.panel), .twin: flat(.twin)], dig = cur.mapValues(digests)
        for s in sides {
            let b = base[s]?["settings"] ?? [:]
            p.settings[s] = dig[s]!.keys.filter { b[$0] != nil && b[$0] != dig[s]![$0] }.sorted()
        }
        let panelKeys = p.settings[.panel]!
        var twinKeys = p.settings[.twin]!
        p.conflicts = panelKeys.filter { k in twinKeys.contains(k) && canon(cur[.panel]![k]) != canon(cur[.twin]![k]) }
        twinKeys.removeAll { panelKeys.contains($0) }
        p.hardware = twinKeys.filter { Settings.hardware.contains($0) }
        twinKeys.removeAll { Settings.hardware.contains($0) }
        p.settings[.twin] = twinKeys
        if let pe = effects[.panel], let te = effects[.twin], hasFxBase(.panel), hasFxBase(.twin) {
            let ch = effectChanges([.panel: pe, .twin: te])
            p.effects = ch.changed
            p.conflicts += ch.conflicts
            if ch.deletes[.panel, default: 0] > SyncEngine.MASS_DELETES {
                p.mass = M("\(ch.deletes[.panel]!) effects removed on the panel at once", "на панели сразу удалено эффектов: \(ch.deletes[.panel]!)")
            }
        }
        if p.settings[.panel]!.count > SyncEngine.MASS_SETTINGS {
            p.mass = M("\(p.settings[.panel]!.count) settings changed on the panel at once", "на панели сразу изменилось настроек: \(p.settings[.panel]!.count)")
        }
        var sig: [String] = []
        for s in sides {
            sig += p.settings[s, default: []].map { "\(s) \($0)=\(dig[s]![$0] ?? "-")" }
            sig += p.effects[s, default: []].map { "\(s) \($0)=\(effects[s]?.fields[$0] ?? "-")" }
        }
        p.sig = sha256Hex(Data(sig.joined(separator: "\n").utf8))
        return p
    }

    /// Sync switched on again with a pair it knows: the second window lists what each side changed while it
    /// was off, whatever that is - even nothing. Unless the consent was kept from the run before (consentKept: the
    /// switch stayed on, the app restarted) and the twin changed nothing meanwhile: then the panel's changes go to
    /// the twin by themselves, with no window (the owner's item 15, 2026-10-01).
    private func resume() throws {
        let plan = resumePlan()
        if let m = plan.mass {
            needDirection = true; holdWhy = m
            log("note", "hold", M("\(m.en): which way?", "\(m.ru): в какую сторону?"), problem: true); askDirectionNow(m); return
        }
        if consentKept && SyncEngine.quietResume(twinSettings: plan.settings[.twin, default: []], twinEffects: plan.effects[.twin, default: []],
                                                  hardware: plan.hardware, conflicts: plan.conflicts) {
            log("note", "resume", M("sync resumes by itself: the switch stayed on, the consent for this pair is kept from before the restart, and the twin changed nothing meanwhile - the panel's changes go to the twin (settings \(plan.settings[.panel, default: []].count), effects \(plan.effects[.panel, default: []].count))",
                                    "синхронизация продолжается сама: переключатель не выключали, согласие для этой пары сохранено с прошлого запуска, двойник за это время не менялся — изменения панели переношу на двойника (настроек \(plan.settings[.panel, default: []].count), эффектов \(plan.effects[.panel, default: []].count))"))
            try carryResume(); return
        }
        if consentKept {
            log("note", "resume", M("the consent for this pair is kept from before the restart, but the twin changed while sync was off: which way - asked as ever",
                                    "согласие для этой пары сохранено с прошлого запуска, но двойник менялся, пока синхронизации не было: направление спрашиваю, как и раньше"))
        }
        askResumeNow(plan)
    }
    /// Whether a resume with a kept consent goes on without the second window: nothing changed on the twin while
    /// sync was off - no setting, no effect, no setting of the panel's hardware, nothing changed on both sides.
    static func quietResume(twinSettings: [String], twinEffects: [String], hardware: [String], conflicts: [String]) -> Bool {
        twinSettings.isEmpty && twinEffects.isEmpty && hardware.isEmpty && conflicts.isEmpty
    }

    /// "From the twin to the panel" at a resume: the changes of both sides since the saved bases, as the
    /// rounds would carry them, with no threshold - the person has seen the list. Effects that a side's
    /// firmware cannot give out are left without a base: the direction is asked the day they can be read.
    private func carryResume() throws {
        log("twin→panel", "resume", M("the changes made while sync was off: the twin's go to the panel, the panel's to the twin",
                                      "изменения, сделанные без синхронизации: двойника переношу на панель, панели — на двойника"))
        try settingsPass(allowMass: true)
        if let p = effects[.panel], let t = effects[.twin] { try effectsPass([.panel: p, .twin: t], allowMass: true); try renameTwinFiles() }
        else { for s in sides { base[s]?["effects"] = nil }; fxAlignFrom = nil }
        try mirrorScreen(from: .panel, fields: SyncEngine.screenFields)
        rebaseScreen(.panel); rebaseScreen(.twin)
        aligned = true; resumed = true; stateDirty = true; fxWaitSince = nil
        retryPending()
        saveState()
    }

    // MARK: the screen

    private func rebaseScreen(_ s: Side) { if let x = screen[s] { base[s, default: [:]]["screen"] = x.fields } }
    /// The whole screen, as a side takes the other's (Screen.fields); the panel's carousel walk is written only
    /// while the panel leads (SyncEngine.leader), the twin's never.
    static let screenFields: Set<String> = ["page", "style", "bright", "off"]

    // MARK: the carousel ("One carousel for both" in the header)

    /// Who leads while the carousel is on: the panel, whenever its carousel is on - walking, or held after a
    /// person's choice; the twin waits for its steps. Nobody while it is off: the carousel's settings are one
    /// for both sides (Settings.walkKey), so the twin's is off too, a round later at most. Never the twin: a
    /// panel that follows would take a write each slot, and each clock style written to it would be saved to
    /// its flash (clock_style.cpp:43-50, 136-142) - where its own carousel only shows the style. A panel whose
    /// build has no carousel (web_panel.cpp:470: 400) never leads, and the twin's walk stays the twin's.
    static func leader(panel: Screen?) -> Side? { panel?.enabled == true ? .panel : nil }

    /// What the twin is given of the leading panel's screen: the page - not a card (each device's own
    /// notifications), not one the twin does not have - and the clock style, when the twin has it; the style
    /// also off the clock page, where the lap's end puts the first one back (clockStyleCarouselNext,
    /// clock_style.cpp:64-78), so the clock comes around in the same style on both. Nothing while the twin
    /// shows a card of its own, or has a page entered with its knob (a show would leave it: main.cpp:891).
    static func followFields(leader p: Screen, follower t: Screen) -> Set<String> {
        guard t.key != "cards", !t.entered else { return [] }
        var f = Set<String>()
        if p.shown != t.shown, p.key != "cards", t.index(key: p.key, name: p.name) != nil { f.insert("page") }
        if p.style != t.style, t.styles.contains(p.style) { f.insert("style") }
        return f
    }

    /// Which of the ASKED screen fields of side S's screen SRC may be written to the other side: all while S is
    /// the leading panel; else not what S's own carousel walks now (Screen.walked) - so the twin's walk never
    /// reaches the panel, whatever asks (a restart, an alignment, a person's change beside it).
    static func writable(_ asked: Set<String>, from s: Side, _ src: Screen, panel: Screen?) -> Set<String> {
        leader(panel: panel) == s ? asked : asked.subtracting(src.walked)
    }

    enum TwinMove: Equatable { case none, follow, hold }
    /// What the twin needs now, the panel leading. FOLLOW: it shows another page or style than the panel.
    /// HOLD: its carousel would walk by itself before the panel's does - the panel is held (a person's choice)
    /// and the twin walks, or its hold ends within holdAhead and sooner than the panel's. A PERSON at the twin:
    /// nothing while their hold lasts - the screen is theirs - and as it is about to end, the panel's screen,
    /// or a hold where the twin already shows it, so the twin walks on with the panel.
    static func twinMove(panel p: Screen, twin t: Screen, person: Bool) -> TwinMove {
        guard leader(panel: p) == .panel, !t.entered, t.key != "cards" else { return .none }
        let differs = !followFields(leader: p, follower: t).isEmpty
        let endsSoon = t.enabled && (t.running || Double(t.holdS) <= holdAhead)
        if person { return endsSoon ? (differs ? .follow : .hold) : .none }
        if differs { return .follow }
        if endsSoon && !p.running && (t.running || t.holdS < p.holdS) { return .hold }
        return .none
    }

    /// Whether a carousel was touched between two reads of its hold: holdS only grows at a touch
    /// (carouselNote: a knob turn or press, the remote, a page shown from the portal or by sync) and else falls
    /// a second a second; rounded up to the second, so a rise of more than 1.5 s over the fall is a touch.
    static func touched(before: Int, at a: Date, now: Int, at b: Date) -> Bool {
        now > 0 && Double(now) > Double(before) - b.timeIntervalSince(a) + 1.5
    }
    /// Each read of the twin's /api/panel (or the answer to a write there): a touch sync did not make - none
    /// written between this read and the one before - is a person at the twin (twinTouch); sync's own write
    /// since that touch clears it.
    private func sawTwin(_ t: Screen, at: Date) {
        // A carousel switched off says holdS 0 (carouselHoldMs): switched on, its hold is not a touch.
        if let h = twinHoldSeen, h.on, t.enabled, twinWroteAt < h.at, SyncEngine.touched(before: h.hold, at: h.at, now: t.holdS, at: at) { twinTouch = at }
        if let p = twinTouch, twinWroteAt > p { twinTouch = nil }
        twinHoldSeen = (t.holdS, at, t.enabled)
    }
    /// A person at the twin now: a touch sync did not make, whose hold has not ended, or a page entered with its knob.
    private var personAtTwin: Bool {
        guard let t = screen[.twin] else { return false }
        return t.entered || (twinTouch != nil && t.holdS > 0)
    }

    /// When the leader's carousel steps next, from one read of its /api/panel: nextS seconds, rounded
    /// (web_panel.cpp:353-364: max(secs - pageS, holdS), pageS cut down to the second, holdS rounded up),
    /// counted from the moment the panel answered - somewhere between SENT and RECEIVED. So the step comes
    /// after sent + nextS - 1 and by received + nextS. nextS 0 or less: due now, at the next pass of loop().
    static func stepWindow(nextS: Int, sent: Date, received: Date) -> (lo: Date, hi: Date) {
        let n = Double(max(nextS, 0))
        return (sent.addingTimeInterval(n - 1), received.addingTimeInterval(n))
    }
    /// Two windows of the same step: what both allow; nil when they do not meet (the carousel was held or
    /// stepped meanwhile: the newer one stands).
    static func narrow(_ a: (lo: Date, hi: Date), _ b: (lo: Date, hi: Date)) -> (lo: Date, hi: Date)? {
        let lo = max(a.lo, b.lo), hi = min(a.hi, b.hi)
        return lo < hi ? (lo, hi) : nil
    }
    /// When to look for the step: in the middle of a wide window (what it finds halves it), else just after its
    /// end, when the step has come.
    static func stepReadAt(_ w: (lo: Date, hi: Date)) -> Date {
        let width = w.hi.timeIntervalSince(w.lo)
        return width > 0.6 ? w.lo.addingTimeInterval(width / 2) : w.hi.addingTimeInterval(stepMargin)
    }

    /// Each read of the panel's /api/panel narrows the window of its carousel's next step (stepWin).
    private func timeStep(_ x: Screen, sent: Date, received: Date) {
        guard x.enabled, let n = x.nextS else { stepWin = nil; stepTries = 0; return }
        let key = "\(x.shown)|\(x.style)"
        var w = SyncEngine.stepWindow(nextS: n, sent: sent, received: received)
        // Not stepped yet when the panel answered: the step is later than the request. A window that does not
        // meet the last one (the carousel was held meanwhile) starts again.
        if let o = stepWin, o.key == key, let m = SyncEngine.narrow((max(o.lo, sent), o.hi), w) { w = m } else { stepTries = 0 }
        stepWin = (w.lo, w.hi, key)
    }
    /// The read that looks for the panel's step while the twin follows it; none after STEP_TRIES in vain for
    /// one step (the screen round goes on every fastEvery whatever happens), none while a person is at the twin.
    private func planStep() {
        stepAt = nil
        guard aligned, SyncEngine.stepPlanned(leader: SyncEngine.leader(panel: screen[.panel]), person: personAtTwin,
                                              strained: strained, tries: stepTries), let w = stepWin else { return }
        stepAt = SyncEngine.stepReadAt((w.lo, w.hi))
    }
    /// Whether the panel's step is looked for between the screen rounds: while it leads, no person is at the
    /// twin, fewer than STEP_TRIES looked in vain - and never while the panel is under strain, when it is read
    /// only every strainedFastEvery: the twin follows it a screen round later then.
    static func stepPlanned(leader: Side?, person: Bool, strained: Bool, tries: Int) -> Bool {
        leader == .panel && !person && !strained && tries < STEP_TRIES
    }

    /// The panel's /api/panel alone, when its carousel's step is due: the step is given to the twin at once,
    /// and this read stands for the next screen round. A page or style the carousel did not put there (it is
    /// held: a person's choice), or a person's choice on the twin, goes to the screen round, at once.
    private func stepRead() throws {
        stepTries += 1
        guard var x = screen[.panel] else { stepAt = nil; return }
        let before = x, sent = Date()
        guard let pn = try device(.panel).get("/api/panel") else { stepAt = nil; return }
        let got = Date()
        x.apply(panel: pn)
        docs[.panel, default: [:]]["/api/panel"] = pn
        screen[.panel] = x; pageReadAt[.panel] = got
        timeStep(x, sent: sent, received: got)
        if x.fields != before.fields {
            if x.running && SyncEngine.leader(panel: x) == .panel, try !twinChosen() {
                if x.shown != before.shown { movedTo[.panel] = x.shown }
                rebaseScreen(.panel)                                      // its walk: nobody's change
                try twinAct(quick: true)
                nextFast = max(nextFast, Date().addingTimeInterval(strained ? SyncEngine.strainedFastEvery : SyncEngine.fastEvery))
            } else {
                nextFast = Date()
            }
        }
        planStep()
    }

    /// The twin's /api/panel, read just before the panel's step is written to it: whether a person chose a
    /// page or style there since the last round, or is at it. Then the screen round comes first and carries
    /// that choice (it holds the panel's carousel); the step would have written over it.
    private func twinChosen() throws -> Bool {
        guard var t = screen[.twin], let b = base[.twin]?["screen"], let tp = try device(.twin).get("/api/panel") else { return false }
        let prev = t
        t.apply(panel: tp)
        docs[.twin, default: [:]]["/api/panel"] = tp
        screen[.twin] = t; pageReadAt[.twin] = Date()
        sawTwin(t, at: Date())
        // A page its renumbered effect list moved is nobody's choice (renumbered): the screen round says so and puts
        // the panel's back; the step waits for it.
        if SyncEngine.renumbered(prev: prev, now: t) { shifted[.twin] = t.shown; return true }
        return personAtTwin || !SyncEngine.screenChanges(t, base: b, wrote: wrote[.twin]).changed.isDisjoint(with: ["page", "style"])
    }

    /// A Lua page that moved with nobody choosing it: the effect list was renumbered under it - an effect uploaded or
    /// removed, and the page index shown stays while another effect now sits at it ("an upload can add a slot and
    /// renumber the ones above it, so the index on screen may already mean a different file", web_panel.cpp
    /// handleLuaUploadChunk). The same page index, another effect at it, another effect list. A person's choice
    /// moves the index.
    static func renumbered(prev: Screen, now: Screen) -> Bool {
        prev.key == "lua" && now.key == "lua" && prev.page == now.page && prev.name != now.name && prev.luaSig != now.luaSig
    }

    /// The twin as the panel leads it (twinMove): the panel's page and style written to it - sync's own write
    /// (wrote), never a change of the twin's - or its carousel held. HOLD also asks for a hold where twinMove
    /// sees nothing to do: the carousel was just switched on at the twin, whose carousel started first and
    /// would step first. Each write holds the twin's carousel for the idle time (panelShowPage -> carouselNote,
    /// main.cpp:883-889), so it does not walk by itself.
    private func twinAct(quick: Bool = false, hold: Bool = false) throws {
        guard let p = screen[.panel], let t = screen[.twin] else { return }
        var move = SyncEngine.twinMove(panel: p, twin: t, person: personAtTwin)
        if move == .none, hold, SyncEngine.leader(panel: p) == .panel, t.enabled { move = .hold }
        switch move {
        case .none: return
        case .follow:
            let f = SyncEngine.followFields(leader: p, follower: t)
            do { try mirrorScreen(from: .panel, fields: f, quick: quick); forgive("follow", .panel, Array(f)) }
            catch { if error is SyncStopped || error is SyncDown || error is SyncRestart || tryAgain("follow", .panel, Array(f)) { throw error } }
        case .hold:
            try holdTwin()
        }
    }

    /// The twin's carousel held from now, as a knob turn would hold it: its own page shown again (panelShowPage
    /// -> carouselNote; the page is not saved anywhere). Not while a page is entered with its knob (a show
    /// leaves it) or a card is shown. Its answer is the twin's screen after it.
    private func holdTwin() throws {
        guard let t = screen[.twin], !t.entered, t.key != "cards" else { return }
        let a = try device(.twin).post("/api/panel", ["show": ["page": t.page]])
        var x = t; x.apply(panel: a); screen[.twin] = x; pageReadAt[.twin] = Date(); rebaseScreen(.twin)
        sawTwin(x, at: Date())
        log("panel→twin", "hold", M("the twin's carousel held \(x.holdS) s: it waits for the panel's step",
                                    "карусель двойника на паузе \(x.holdS) с: ждёт шага панели"))
    }

    private func fastRound() throws {
        var fresh: [Side: Screen] = [:]
        for s in sides { fresh[s] = try readScreen(s) }                 // a restart throws SyncRestart (restarts)
        try noRestartPending()
        // An uptime that jumped ahead: the pair is checked before anything is written.
        var jumped = false
        for s in sides {
            guard let prev = screen[s], let at = screenAt[s] else { continue }
            let x = fresh[s]!
            if prev.luaSig != x.luaSig { fxDue = true }
            if SyncEngine.renumbered(prev: prev, now: x) { shifted[s] = x.shown }
            if x.uptime > prev.uptime + Int(Date().timeIntervalSince(at)) + 30 { jumped = true }   // 30 s of slack: our choice
        }
        if jumped { try verifyIdentity() }
        var prevShown: [Side: String] = [:]
        for s in sides { prevShown[s] = screen[s]?.shown; screen[s] = fresh[s]; screenAt[s] = Date() }
        if base[.panel]?["screen"] == nil || base[.twin]?["screen"] == nil { rebaseScreen(.panel); rebaseScreen(.twin); return }
        // One carousel for both: its settings and the pages it visits, carried in this round; the side written
        // to is read again, and what it shows then is nobody's change (afterWrite). Not after a restart until the
        // settings round has compared everything: a device whose flash was erased comes back with the defaults,
        // a massive change the person is asked about (MASS_SETTINGS), not a carousel to carry.
        var switchedOn = false
        if !walkHeld {
            let wasOn = screen[.panel]?.enabled == true
            try settingsPass(allowMass: false, only: Settings.walkKey)
            // Switched on at the twin and now at the panel: the twin's carousel started first and would step
            // first; held (twinAct, after the person's changes below), it waits for the panel's step.
            switchedOn = !wasOn && screen[.panel]?.enabled == true
        }
        let cur = screen                                    // after a reboot's own handling and the carousel's settings
        var changed: [Side: Set<String>] = [:]
        for s in sides {
            let r = SyncEngine.screenChanges(cur[s]!, base: base[s]!["screen"]!, wrote: wrote[s])
            changed[s] = r.changed; wrote[s] = r.waiting
            // Who put the page there: a person (a change of the side's own) - or sync, its carousel, a renumbered list.
            if r.changed.contains("page") { movedTo[s] = nil }
            else if let ps = prevShown[s], ps != cur[s]!.shown { movedTo[s] = cur[s]!.shown }
        }
        // The twin's page moved by its effect list renumbered under it (an effect uploaded there by a person): nobody
        // chose it - not carried; the twin shows the panel's page again below. The panel's own screen, whatever moved
        // it, is what the twin shows.
        var backToPanel = false
        if let sh = shifted[.twin], sh == cur[.twin]!.shown, changed[.twin]!.contains("page") {
            changed[.twin]!.remove("page"); movedTo[.twin] = sh
            backToPanel = cur[.panel]!.shown != sh && cur[.twin]!.index(key: cur[.panel]!.key, name: cur[.panel]!.name) != nil
            log("note", "screen", M("the twin's effect list was renumbered and its page moved to \(cur[.twin]!.name) by itself: nobody chose it - not carried to the panel\(backToPanel ? "; the twin shows the panel's \(cur[.panel]!.name) again" : "")",
                                    "список эффектов двойника перенумеровался, и его страница сама сдвинулась на \(cur[.twin]!.name): это не выбор человека — на панель не переношу\(backToPanel ? "; двойнику возвращаю страницу панели \(cur[.panel]!.name)" : "")"))
        }
        shifted = [:]
        // Both changed one field: the page to the one changed last (smaller pageS), the rest to the panel.
        for f in changed[.panel]!.intersection(changed[.twin]!).sorted() where cur[.panel]!.fields[f] != cur[.twin]!.fields[f] {
            if f == "page" && cur[.twin]!.pageS < cur[.panel]!.pageS {
                changed[.panel]!.remove(f)
                log("twin→panel", "conflict", M("conflict: the twin's taken (changed later) - screen page", "конфликт: взят двойник (изменён позже) — экран, страница"))
            } else {
                changed[.twin]!.remove(f)
                log("panel→twin", "conflict", M("conflict: the panel's taken - screen \(f)", "конфликт: взята панель — экран, \(f)"))
            }
        }
        // The other side was read a moment ago, in this round: it stands for the read before the write (quick), and the
        // answer to the write for the read after it - two reads fewer on the panel for each change carried to it.
        for s in sides {
            let c = changed[s]!
            if !c.isEmpty {
                do { try mirrorScreen(from: s, fields: c, quick: true); forgive("screen", s, Array(c)) }
                catch { if error is SyncStopped || error is SyncDown || error is SyncRestart || tryAgain("screen", s, Array(c)) { throw error } }   // the base stays: seen again
            }
            rebaseScreen(s)
        }
        if backToPanel { try mirrorScreen(from: .panel, fields: ["page"], quick: true) }
        // Said once a round has read the twin's uptime too: a restart also starts its carousel's hold afresh.
        if personAtTwin != personSaid {
            personSaid = personAtTwin
            if personSaid, SyncEngine.leader(panel: screen[.panel]) == .panel {
                log("note", "person", M("a person at the twin: the panel's steps wait until the twin's carousel would walk again",
                                        "у двойника человек: шаги панели жду, пока карусель двойника снова не пойдёт"))
            }
        }
        // The panel leads: the twin shows what it shows, and its carousel does not walk ahead of the panel's.
        // A person's page or style from the twin went to the panel just now and holds it from now: the twin's
        // hold, older, is renewed before it ends (twinMove), so the panel's carousel goes on first.
        try twinAct(hold: switchedOn)
        // Inside the page both show: the gestures this round's reads gave, the effect's clicks.
        try insidePass()
    }

    /// The twin's page or console told of a person there (takeWake): the twin alone is read, and what a person changed
    /// there is carried as the screen round carries it, the panel's last reading standing for its own (mirrorScreen
    /// QUICK: read within fastEvery + 1 s, or kept by its events). Anything the screen round must judge - its effect list
    /// renumbered or changed, the panel changed the same thing, no bases yet, a restart - goes to that round, now.
    private func twinRound() throws {
        guard !needScreenRound, let prev = screen[.twin], let b = base[.twin]?["screen"], let bp = base[.panel]?["screen"], let ps = screen[.panel] else {
            nextFast = Date(); return
        }
        let t = try readScreen(.twin); screenAt[.twin] = Date()
        try noRestartPending()
        screen[.twin] = t
        if SyncEngine.renumbered(prev: prev, now: t) { shifted[.twin] = t.shown }      // as the screen round would see it
        if prev.luaSig != t.luaSig { fxDue = true; nextFast = Date(); return }
        let r = SyncEngine.screenChanges(t, base: b, wrote: wrote[.twin])
        wrote[.twin] = r.waiting
        if r.changed.contains("page") { movedTo[.twin] = nil } else if prev.shown != t.shown { movedTo[.twin] = t.shown }
        if !r.changed.isEmpty {
            if !SyncEngine.screenChanges(ps, base: bp, wrote: wrote[.panel]).changed.isDisjoint(with: r.changed) { nextFast = Date(); return }
            do { try mirrorScreen(from: .twin, fields: r.changed, quick: true); forgive("screen", .twin, Array(r.changed)) }
            catch { if error is SyncStopped || error is SyncDown || error is SyncRestart || tryAgain("screen", .twin, Array(r.changed)) { throw error } }
            rebaseScreen(.twin)
        }
        try insidePass()
    }

    /// The screen fields side CUR changed since its BASE, as a person's change to carry: not the page or the
    /// style its carousel walks now (Screen.walked), and not what sync itself wrote there (WROTE) once the side
    /// shows it. Also what of WROTE still waits: a field the side still shows as it was (the write may land
    /// yet). A field that shows a third value is the side's own change, and nothing waits for it any more.
    static func screenChanges(_ cur: Screen, base: [String: String], wrote: [String: String]?) -> (changed: Set<String>, waiting: [String: String]?) {
        let f = cur.fields
        var c = Set(f.keys.filter { f[$0] != base[$0] }).subtracting(cur.walked)
        guard let w = wrote else { return (c, nil) }
        for (k, v) in w where f[k] == v { c.remove(k) }
        let rest = w.filter { f[$0.key] != $0.value && f[$0.key] == base[$0.key] }
        return (c, rest.isEmpty ? nil : rest)
    }

    /// A write that failed is tried again in the next rounds, three times at most; true while it may be.
    private func tryAgain(_ kind: String, _ s: Side, _ keys: [String]) -> Bool {
        var again = false
        for k in keys {
            let id = "\(kind)|\(s)|\(k)"
            failures[id, default: 0] += 1
            if failures[id]! < 3 { again = true } else { failures[id] = nil }
        }
        return again
    }
    private func forgive(_ kind: String, _ s: Side, _ keys: [String]) { for k in keys { failures["\(kind)|\(s)|\(k)"] = nil } }

    /// After a round's writes: the source side's changes that were not written keep their old base, so they
    /// are seen and tried again - always when a device did not answer or sync was stopped, three times for
    /// a refusal.
    private func revert(_ kind: String, _ s: Side, _ keys: [String], _ old: [String: String], _ error: Error) {
        let transient = error is SyncDown || error is SyncStopped || error is SyncWait || error is SyncRestart
        if transient || tryAgain(kind, s, keys) { for k in keys { base[s, default: [:]][kind, default: [:]][k] = old[k] } }
        else { log("error", kind, M("given up after three attempts: \(keys.map(Settings.shown).joined(separator: ", "))",
                                    "брошено после трёх попыток: \(keys.map(Settings.shown).joined(separator: ", "))"), problem: true) }
    }

    private func rebooted(_ s: Side, _ x: Screen) {
        // Nothing of a restart is written to the panel. The twin takes the panel's screen; while the panel's
        // carousel is on, the page and style it walks too - the twin follows it (leader).
        let leads = SyncEngine.leader(panel: s == .panel ? x : screen[.panel]) == .panel
        let panelMsg = leads ? M("its carousel leads: the twin shows what it shows", "ведёт её карусель: двойник показывает то же")
                             : M("its reset screen is not mirrored", "её сброшенный экран не переношу")
        let twinMsg = leads ? M("it takes the panel's screen, the page and style of its carousel too: the twin follows it",
                                "он повторяет экран панели, и страницу со стилем её карусели: двойник идёт за ней")
                            : M("it takes the panel's screen", "он повторяет экран панели")
        let m = s == .panel ? panelMsg : twinMsg
        log("note", "reboot", M("\(s.word.en) restarted (uptime \(x.uptime) s): " + m.en,
                                (s == .panel ? "панель перезагрузилась" : "двойник перезагрузился") + " (uptime \(x.uptime) с): " + m.ru))
        screen[s] = x; rebaseScreen(s); wrote[s] = nil               // what it shows now is its reset screen
        movedTo[s] = x.shown                                          // nobody chose it: its effect's clicks so far are history
        if s == .twin { twinTouch = nil; twinHoldSeen = nil }       // its carousel starts again
        // The settings round first (fastRound), after the screen round (needScreenRound): it compares everything,
        // and gives the twin back what its restart lost (twinLost, settingsChanges).
        walkHeld = true; nextSlow = Date()
        fwWatch = Date().addingTimeInterval(5)
        caps[s] = nil
        if s == .twin, aligned { twinFollowsPanel() }
    }

    /// The twin takes the panel's screen (after the twin restarted, after a firmware sync sent it was confirmed).
    /// A failure is said, not thrown: the twin keeps its own screen until the next change on either side.
    private func twinFollowsPanel() {
        do { try mirrorScreen(from: .panel, fields: SyncEngine.screenFields) }
        catch is SyncStopped {}
        catch is SyncRestart {}                                           // restartSeen: dealt with next (restarts)
        catch { log("note", "screen", M("the twin did not take the panel's screen: " + describe(error).en, "двойник не принял экран панели: " + describe(error).ru)) }
    }

    /// Makes the other side's screen show FIELDS of side S's, then reads it again as its base. The panel's
    /// carousel walk is written only while the panel leads (leader), the twin's never; what is written is kept
    /// in `wrote` until it is read back. QUICK (the panel's step, a datagram's change, the screen round that has just read
    /// both; page and style only): the other side's /api/panel read within fastEvery + 1 s stands for a read before the
    /// write - its page numbers - and the answer to the write, the same document as GET /api/panel, for the read after it.
    private func mirrorScreen(from s: Side, fields asked: Set<String>, quick: Bool = false) throws {
        try noRestartPending()
        let o = s.other
        guard let src = screen[s] else { return }
        let leads = SyncEngine.leader(panel: screen[.panel]) == s
        let fields = SyncEngine.writable(asked, from: s, src, panel: screen[.panel])
        guard !fields.isEmpty else { return }
        // Read within fastEvery + 1 s - or, while its own events come and none moved it since, within freshLive (a long
        // round reads no screen meanwhile).
        let age = pageReadAt[o].map { Date().timeIntervalSince($0) } ?? .infinity
        let fresh = quick && fields.isSubset(of: ["page", "style"]) && (age < SyncEngine.fastEvery + 1 || (age < SyncEngine.freshLive && live(o) && !stale.contains(o)))
        var dst = fresh ? screen[o]! : try readScreen(o)
        var body: J = [:], did: [String] = [], expect: [String: String] = [:]
        if fields.contains("page"), src.shown != dst.shown {
            if src.key == "cards" {
            } else if let i = dst.index(key: src.key, name: src.name) {
                body["show"] = ["page": i]; expect["page"] = src.shown; did.append(M("page \(src.name)", "страница \(src.name)").text(lang))
                movedTo[o] = src.shown                                 // its effect's clicks so far are nobody's (reconcileClicks)
            } else {
                noteOnce("nopage-\(o)-\(src.shown)", M("\(o.word.en) has no page \(src.name)", "\(o.on) нет страницы \(src.name)"))
            }
        }
        if fields.contains("style"), src.style != dst.style {
            if dst.styles.contains(src.style) {
                body["style"] = src.style; expect["style"] = "\(src.style)"; did.append(M("clock style \(src.style)", "стиль часов \(src.style)").text(lang))
                // style moves to the clock page (panelShowStyle, main.cpp:906-909): keep the page meant - the
                // target's own when the page is not written.
                if body["show"] == nil, src.key != "clock" || !fields.contains("page"), dst.key != "cards" { body["show"] = ["page": dst.page] }
            } else {
                noteOnce("style-\(o)-\(src.style)", M("\(o.word.en) has no clock style \(src.style)", "\(o.at) нет стиля часов \(src.style)"))
            }
        }
        // Kept before each write: its answer may be lost after the work was done.
        var answer: J?
        if !body.isEmpty {
            wrote[o, default: [:]].merge(expect) { $1 }
            answer = try device(o).post("/api/panel", body)            // on the twin: sync's hold (willWrite)
        }
        var offNow = dst.off, acted = false
        if fields.contains("bright"), src.bright != dst.bright {
            wrote[o, default: [:]]["bright"] = "\(src.bright)"
            try device(o).action("/api/display/brightness", [("value", "\(src.bright)")])
            offNow = src.bright == 0                                   // display.cpp:193
            did.append(M("brightness \(src.bright)%", "яркость \(src.bright)%").text(lang)); acted = true
        }
        if fields.contains("off") || fields.contains("bright"), src.off != offNow {
            wrote[o, default: [:]]["off"] = src.off ? "1" : "0"
            try device(o).action(src.off ? "/api/display/off" : "/api/display/on")
            did.append(src.off ? M("screen off", "экран выключен").text(lang) : M("screen on", "экран включён").text(lang)); acted = true
        }
        guard !did.isEmpty else { return }
        if fresh, !acted, let a = answer, a.o("now") != nil {
            dst.apply(panel: a); pageReadAt[o] = Date()
            if o == .twin { sawTwin(dst, at: Date()) }
        } else { dst = try readScreen(o); screenAt[o] = Date() }
        screen[o] = dst; rebaseScreen(o); wrote[o] = nil
        log(s.arrow, leads && !asked.isDisjoint(with: src.walked) ? "walk" : "screen", M(did.joined(separator: ", "), did.joined(separator: ", ")))
    }

    // MARK: instant events ("Instant events" in the header)

    // Our choice, not measured: the subscription is renewed every listenEvery (the firmware keeps it 60 s); a person's
    // datagram within crossWindow of sync's write to that side goes to the screen round; gestures inside a page go
    // gestureGap apart (the firmware merges /api/ir/do presses closer than about 0.3 s: the design's E4, E6); clicks
    // with no person's spacing to keep go clickGap apart (past the 0.45 s of a double press); an effect's first frame
    // is waited for openWait at most; a side whose own events come, none of which moved it, is taken as read for a
    // carry for freshLive (mirrorScreen QUICK; the screen round reads it every fastEvery anyway, a long round not).
    static let listenEvery: TimeInterval = 25, crossWindow: TimeInterval = 1, gestureGap: TimeInterval = 0.35
    static let clickGap: TimeInterval = 0.5, openWait: TimeInterval = 6, freshLive: TimeInterval = 12

    /// A person's doing ("by", sync_events.h): the knob, the remote, a request without X-Twin-Sync (the portal, Home
    /// Assistant). Not "sync" (its own write coming back), "carousel", "schedule" (the night) or "auto" (no cause noted).
    static func person(_ by: String) -> Bool { by == "knob" || by == "ir" || by == "http" }
    /// Whether a screen datagram numbered SEQ was sent before the side's screen was read last, at READ (now.seq of GET
    /// or POST /api/panel): a state gets its number when it is first sent, and a read gives the last number sent
    /// (sync_events.cpp syncEventsLoop, syncPanelJson) - so a datagram numbered READ or less holds what the read holds,
    /// or older, and is nothing to carry. Without this, one drained after a screen round carried back a page the side
    /// had left (the stand, 2026-10-01 07:49:59: the twin's seq 4, FLIP DOT CLOCK by http, drained after the round had
    /// read it on FLOW at seq 5 - the panel written FLOW, FLIP DOT CLOCK, FLOW in 0.6 s, with nobody at it). A device
    /// that restarted numbers afresh (far below: event) - not stale then.
    static func staleScreen(seq q: Int?, read r: Int?) -> Bool {
        guard let q, let r else { return false }
        return q <= r && q + 100 >= r
    }

    /// The IPv4 address of HOST ("192.168.4.89", "panel.local", "localhost"), nil when it has none.
    static func ipv4(of host: String) -> String? {
        var a = in_addr()
        if inet_pton(AF_INET, host, &a) == 1 { return host }
        var hints = addrinfo(); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM
        var res: UnsafeMutablePointer<addrinfo>?
        guard getaddrinfo(host, nil, &hints, &res) == 0, let r = res, let sa = r.pointee.ai_addr else { return nil }
        defer { freeaddrinfo(res) }
        return sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { p -> String? in
            var x = p.pointee.sin_addr; var buf = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            return inet_ntop(AF_INET, &x, &buf, socklen_t(INET_ADDRSTRLEN)).map { _ in String(cString: buf) }
        }
    }

    /// Where SIDE's datagrams come from and the port it is asked to send them to. A device on the network sends from
    /// its own address (port 4210) to this app's port. A device behind the engine's NAT (127.0.0.1: the twin without the
    /// home network; a test's "panel") sees this Mac as 10.0.2.2, and a datagram it sends there reaches the Mac only as
    /// an answer on its forwarded UDP port FWD (--hostfwd udp:FWD-4210): the engine sends a guest's datagram from 4210 to
    /// the gateway's port FWD to the host peer that sent to FWD last (esp-soc/src/nat.rs udp_out). So it is asked to send
    /// to FWD, and this app's socket sends an empty datagram to FWD first (EventListener.prime).
    private func eventRoute(_ s: Side) -> (host: String, port: UInt16?, say: Int)? {
        guard let mine = listener?.port, mine > 0, let d = dev[s] else { return nil }
        let host = String(d.address.split(separator: ":").first ?? "")
        // A name (panel.local) is looked up when a run of subscriptions starts, not at each renewal.
        let known = ev[s].flatMap { $0.since != nil && !$0.host.isEmpty ? $0.host : nil }
        guard let ip = known ?? SyncEngine.ipv4(of: host) else { return nil }
        guard ip.hasPrefix("127.") else { return (ip, nil, Int(mine)) }
        let fwd: Int? = s == .twin ? locked({ _twinUdp }) : testInt("syncPanelUdpForTesting")
        guard let fwd, (1024...65535).contains(fwd) else { return nil }
        return ("127.0.0.1", UInt16(fwd), fwd)
    }

    private func sayOnce(_ key: String, _ m: Msg) { if noted.insert(key).inserted { log("note", "events", m) } }

    /// Both sides asked for their events (POST /api/sync/listen {"port"}), every listenEvery - only once both of the
    /// person's confirmations are given: before them sync writes nothing, a subscription included.
    private func listenIfDue() {
        guard Date() >= nextListen else { return }
        nextListen = Date().addingTimeInterval(SyncEngine.listenEvery)
        if listener == nil && !listenerFailed {
            listener = EventListener { [weak self] d, ip, port in self?.received(d, ip, port) }
            if listener == nil {
                listenerFailed = true
                log("note", "events", M("no UDP socket for the instant events: both sides are read every 3 s", "нет UDP-сокета для мгновенных событий: обе стороны читаю раз в 3 с"))
            }
        }
        guard listener != nil else { return }
        for s in sides { listen(s) }
    }

    /// One side's subscription, renewed. A run of them starts with its end ({"stop":true}) and a new one, so the
    /// firmware sends the screen as it is at once (sync_events.cpp syncListen: a renewal sends nothing) - the datagram
    /// that tells they reach this app (live). 404: a firmware before 2.7.13 - asked again in 5 min; 409: both places
    /// taken - asked again at the next renewal. Either way its screen is read every 3 s, as before.
    private func listen(_ s: Side) {
        var e = ev[s] ?? Ev()
        guard Date() >= e.noRoute else { return }
        guard let r = eventRoute(s) else {
            sayOnce("ev-route-\(s)", M("\(s.word.en): its instant events cannot reach this app (behind the engine's NAT with no forwarded UDP port): it is read every 3 s" + (s == .twin ? ", and the twin's page and console tell of a person there" : ""),
                                       "\(s.word.ru): \(s.its) мгновенные события сюда не дойдут (за NAT движка без проброшенного UDP-порта): читаю раз в 3 с" + (s == .twin ? ", а о человеке у двойника говорят его страница и консоль" : "")))
            return
        }
        if e.host != r.host || e.port != r.port || e.say != r.say { e = Ev(inputSeq: e.inputSeq); e.host = r.host; e.port = r.port; e.say = r.say }
        // Every renewal: whoever sent there last gets the answers. The engine takes the empty datagram on its next pass:
        // the first of a run, sent the moment the subscription is taken, was lost to a NAT with no peer yet (2026-10-01,
        // the stand) - a moment before asking.
        if let p = r.port { listener?.prime(p); if e.since == nil { Thread.sleep(forTimeInterval: 0.1) } }
        do {
            let d = device(s), asked = Date()                         // its first datagram may come before the answer
            if e.since == nil { _ = try d.send("POST", "/api/sync/listen", body: try jsonData(["port": r.say, "stop": true]), type: "application/json", timeout: 5, attempts: 1, quiet: true) }
            let a = try d.send("POST", "/api/sync/listen", body: try jsonData(["port": r.say]), type: "application/json", timeout: 5, attempts: 1, quiet: true)
            switch a.status {
            case 200:
                if e.since == nil {
                    e.since = asked; e.seq = a.object?.i("seq") ?? 0
                    log("note", "events", M("\(s.word.en): subscribed to its instant events (UDP port \(r.say)): its changes are carried at once",
                                            "\(s.word.ru): подписался на \(s.its) мгновенные события (UDP-порт \(r.say)): \(s.its) изменения переношу сразу"))
                }
                e.renewed = Date()
                let sub = (d.address, r.say)
                locked { _subs[s] = sub }
            case 404:
                e.since = nil; e.renewed = nil; e.noRoute = Date().addingTimeInterval(300)
                sayOnce("ev-404-\(s)", M("\(s.word.en): no instant events (a firmware before 2.7.13, no /api/sync/listen): its screen is read every 3 s",
                                         "\(s.word.ru): мгновенных событий нет (прошивка до 2.7.13, нет /api/sync/listen): \(s.its) экран читаю раз в 3 с"))
            default:
                e.since = nil; e.renewed = nil
                sayOnce("ev-\(a.status)-\(s)", M("\(s.word.en): POST /api/sync/listen: HTTP \(a.status) \(a.why): it is read every 3 s, and asked again every \(Int(SyncEngine.listenEvery)) s",
                                                 "\(s.word.ru): POST /api/sync/listen: HTTP \(a.status) «\(a.why)»: читаю раз в 3 с и прошу снова каждые \(Int(SyncEngine.listenEvery)) с"))
            }
        } catch {
            e.since = nil; e.renewed = nil                            // switched off, or no answer: the next renewal starts afresh
        }
        ev[s] = e
    }

    /// Whether SIDE's datagrams reach this app now: subscribed within the firmware's 60 s, and heard since this run of
    /// subscriptions began.
    private func live(_ s: Side) -> Bool {
        guard let e = ev[s], let since = e.since, let r = e.renewed, Date().timeIntervalSince(r) < 60, let h = e.heard else { return false }
        return h >= since
    }

    /// The end of a subscription: sent whatever the switch says, one try - it only stops the device sending.
    static func unlisten(_ d: Device, port: Int) {
        guard let body = try? jsonData(["port": port, "stop": true]) else { return }
        _ = try? d.send("POST", "/api/sync/listen", body: body, type: "application/json", timeout: 1.5, attempts: 1, quiet: true, unstoppable: true)
    }
    private func unlistenAll() {
        for s in sides { if let e = ev[s], e.since != nil, let d = dev[s] { SyncEngine.unlisten(d, port: e.say) } }
        locked { _subs = [:] }
        ev = [:]
    }

    /// The listener's thread: a datagram, kept for the worker, which is woken.
    private func received(_ d: Data, _ ip: String, _ port: UInt16) {
        locked {
            _datagrams.append((d, ip, port, Date()))
            if _datagrams.count > 256 { _datagrams.removeFirst(_datagrams.count - 256) }
        }
        wakeSem.signal()
    }

    /// Between any two requests of a round that takes a while (the effects', the settings', the firmware's, a script's
    /// SHA-256 on its schedule; LongRounds): what came meanwhile is carried now, not after the round - what this carries is
    /// the screen's and the clicks', which those rounds do not decide, and a side whose effect list such a round wrote is
    /// read again before anything is carried to it (listMoved). Not inside itself, not before both confirmations. Not in
    /// the screen round: it has read both screens and decides on them.
    private func serveEvents() throws {
        guard aligned, !serving, locked({ !_datagrams.isEmpty }) || insideQ.first.map({ $0.at <= Date() }) == true else { return }
        serving = true; defer { serving = false }
        try drainEvents(); try runInside()
    }
    /// A read of side SIDE gives way now (Device.yields; nil: a read on the schedule, hashTick): inside a round that takes a
    /// while, not inside a serving, both confirmations given, and a datagram waits for it (eventWaits). From the worker only.
    private func yieldNow(from side: Side?) -> Bool { rounds.depth > 0 && !rounds.serving && aligned && eventWaits(from: side) }
    /// F as a round that takes a while: the instant events are served between any two of its requests (LongRounds).
    private func long<T>(_ f: () throws -> T) rethrows -> T { try rounds.run(f) }
    /// An effect written to side O (removed, uploaded, its walk switched) can renumber its list under its page: its screen
    /// as last read is not what it shows. Read again before anything is carried to it or done inside it (stale; mirrorScreen
    /// reads a side whose page numbers are not known fresh).
    private func listMoved(_ o: Side) { stale.insert(o); pageReadAt[o] = nil }

    /// The twin's page or console told of a person there: the screen round at AFTER from now.
    private func wakeFromTwin(after: TimeInterval) {
        let t = Date().addingTimeInterval(after)
        locked { if _wakeAt.map({ t < $0 }) ?? true { _wakeAt = t } }
        wakeSem.signal()
    }
    /// Whether that round is due now: not while the twin's own events tell of it (they come first), not more than
    /// one in 0.3 s, none while the panel is under strain (it is read every strainedFastEvery then).
    private func takeWake(_ now: Date) -> Bool {
        guard let w = locked({ _wakeAt }), now >= w else { return false }
        locked { _wakeAt = nil }
        guard !live(.twin), !strained else { return false }
        if now.timeIntervalSince(lastWoken) < 0.3 { let t = lastWoken.addingTimeInterval(0.3); locked { _wakeAt = t }; return false }
        return true
    }

    /// The datagrams that came, in order, each from the side it came from. Older than 5 s: left to the rounds.
    private func drainEvents() throws {
        let got = locked { () -> [(data: Data, ip: String, port: UInt16, at: Date)] in let g = _datagrams; _datagrams = []; return g }
        let now = Date()
        for g in got where now.timeIntervalSince(g.at) < 5 {
            guard let s = sides.first(where: { ev[$0]?.since != nil && ev[$0]?.host == g.ip && (ev[$0]?.port == nil || ev[$0]?.port == g.port) }),
                  let o = (try? JSONSerialization.jsonObject(with: g.data)) as? J else { continue }
            if let f = eventFilter, !f(s, o) { continue }
            try event(s, o, at: g.at)
        }
        for s in sides {
            guard let e = ev[s], let since = e.since, (e.heard ?? .distantPast) < since, now.timeIntervalSince(since) > 3 else { continue }
            sayOnce("ev-deaf-\(s)", M("\(s.word.en) took the subscription, but its datagrams do not reach this app: it is read every 3 s" + (s == .twin ? ", and the twin's page and console tell of a person there" : ""),
                                      "\(s.word.ru) \(s == .panel ? "приняла" : "принял") подписку, но \(s.its) датаграммы сюда не доходят: читаю раз в 3 с" + (s == .twin ? ", а о человеке у двойника говорят его страница и консоль" : "")))
        }
        if clicksDue, stale.isEmpty { try reconcileClicks() }
    }

    /// One datagram. A gap in seq: one was lost - both sides are read now, the gestures with them (?input=). A seq far
    /// below the last: numbered afresh, the device restarted (its uptime tells the rounds).
    private func event(_ s: Side, _ o: J, at: Date) throws {
        var e = ev[s]!
        e.heard = at
        if let q = o.i("seq") {
            if q + 100 < e.seq { e.seq = q; e.inputSeq = nil }
            else {
                if e.seq > 0, q > e.seq + 1 { nextFast = Date() }
                e.seq = max(e.seq, q)
            }
        }
        ev[s] = e
        switch o.s("t") {
        case "screen": try screenEvent(s, o, at: at)
        case "input": try inputEvent(s, o, at: at)
        default: break
        }
    }

    /// A screen datagram: the page, the style, the brightness, on/off, the stop inside a page, the effect's clicks.
    /// A person's change is carried at once (carryNow); the leading panel's carousel step is given to the twin
    /// (followStep); sync's own write coming back and the night schedule change nothing; anything else - "auto", or
    /// on/off, which the datagram gives with the schedule's - goes to the screen round, now. One sent before the side's
    /// screen was last read changes nothing (staleScreen).
    private func screenEvent(_ s: Side, _ o: J, at: Date) throws {
        guard let before = screen[s], base[s]?["screen"] != nil else { return }
        if SyncEngine.staleScreen(seq: o.i("seq"), read: before.seq) { return }
        if let q = o.i("seq") { screen[s]!.seq = q }                        // the state known now is this datagram's
        let page = o.i("page") ?? before.page, key = o.s("key") ?? before.key, name = o.s("name") ?? before.name
        if let f = o.o("fx") {
            let id = f.i("id") ?? -1, run = f.i("run") ?? 0
            let fx = Fx(id: id, open: before.fx.map { $0.id == id && $0.run == run && $0.open } ?? false, run: run, clicks: f.i("clicks") ?? 0)
            // Taken only as the effect of the datagram's own page: the datagram of a page change may still carry the
            // effect before it (Screen.fxHere), whose clicks are not this page's.
            var probe = before; probe.page = page; probe.key = key; probe.fx = fx
            if fx != before.fx, id < 0 || probe.fxHere != nil { screen[s]!.fx = fx; clicksDue = true }
        }
        if let en = o.b("entered"), en != before.entered { screen[s]!.entered = en; enteredExpect[s] = nil }
        let style = o.i("style") ?? before.style, bright = o.i("bright") ?? before.bright
        let moved = page != before.page || key != before.key || name != before.name || style != before.style || bright != before.bright
        let offMoved = (o.b("off") ?? before.off) != before.off
        guard moved || offMoved else { return }
        let by = o.s("by") ?? ""
        if by == "sync" || by == "schedule" { return }
        if by == "carousel" {
            if s == .panel, !offMoved, bright == before.bright, SyncEngine.leader(panel: before) == .panel { try followStep(page: page, key: key, name: name, style: style) }
            else { stale.insert(s); nextFast = Date() }
            return
        }
        guard SyncEngine.person(by), !offMoved else { stale.insert(s); nextFast = Date(); return }
        if s == .twin { twinTouch = at }
        if page != before.page || name != before.name { movedTo[s] = nil }      // a person's page
        if try !carryNow(s, page: page, key: key, name: name, style: style, bright: bright) { stale.insert(s); nextFast = Date() }
    }

    /// A person's page, style or brightness from SIDE's datagram, carried at once. The datagram is the side's screen,
    /// so the side is not read; the other side is written as the screen round would write it (mirrorScreen, its answer
    /// read as its screen). False: the screen round decides instead - a write to this side within crossWindow (the
    /// datagram may come from before it), the other side changed the same thing, the page is not where this side's
    /// list has it, a failure or a restart waits for that round.
    private func carryNow(_ s: Side, page: Int, key: String, name: String, style: Int, bright: Int) throws -> Bool {
        let o = s.other
        guard !needScreenRound, restartSeen.isEmpty, var x = screen[s], let b = base[s]?["screen"], let bo = base[o]?["screen"], let y = screen[o],
              Date().timeIntervalSince(lastWrite[s] ?? .distantPast) > SyncEngine.crossWindow, x.index(key: key, name: name) == page else { return false }
        x.page = page; x.key = key; x.name = name; x.style = style; x.bright = bright
        x.running = false; x.pageS = 0                                  // a person's gesture holds its carousel (carouselNote)
        if x.enabled { x.holdS = max(x.holdS, x.idleS) }
        let changed = Set(x.fields.keys.filter { x.fields[$0] != b[$0] })
        if !SyncEngine.screenChanges(y, base: bo, wrote: wrote[o]).changed.isDisjoint(with: changed) { return false }
        screen[s] = x
        guard !changed.isEmpty else { return true }
        do { try mirrorScreen(from: s, fields: changed, quick: true) }
        catch { if error is SyncStopped || error is SyncDown || error is SyncRestart { throw error }; return false }   // the base stays: seen again
        rebaseScreen(s)
        return true
    }

    /// The leading panel's carousel stepped (its datagram, "carousel"): the step is given to the twin at once, as
    /// stepRead gives it - nothing is read of the panel; of the twin only where its own events do not come.
    private func followStep(page: Int, key: String, name: String, style: Int) throws {
        guard var x = screen[.panel] else { return }
        x.page = page; x.key = key; x.name = name; x.style = style
        x.running = true; x.pageS = 0; x.holdS = 0
        if x.shown != screen[.panel]?.shown { movedTo[.panel] = x.shown }
        screen[.panel] = x
        stepAt = nil; stepWin = nil; stepTries = 0
        rebaseScreen(.panel)                                              // its walk: nobody's change
        if !live(.twin), try twinChosen() { nextFast = Date(); return }   // a person's choice there goes first
        try twinAct(quick: true)
    }

    /// A gesture (an input datagram, or one of the gestures GET /api/panel?input= gave), taken once by its seq. A
    /// person's press on an effect is counted by now.fx (reconcileClicks); on a page with a stop inside, the gesture is
    /// made on the other side (gesture).
    private func inputEvent(_ s: Side, _ o: J, at: Date) throws {
        guard let q = o.i("seq") else { return }
        if let last = ev[s]?.inputSeq, q <= last { return }
        ev[s, default: Ev()].inputSeq = q
        let by = o.s("by") ?? "", kind = o.s("kind") ?? "", entered = o.b("entered") ?? false
        guard SyncEngine.person(by), let i = o.i("page"), let pg = screen[s]?.page(i) else { return }
        if pg.key == "lua" {
            if kind == "press" { presses[s, default: []].append(at); presses[s] = Array(presses[s]!.suffix(8)); clicksDue = true }
            return
        }
        try gesture(s, kind: kind, key: pg.key, name: pg.name, entered: entered)
    }

    /// What is carried inside a page, by its key: an effect's clicks; the stops of the world clock and the flight board
    /// (in, turns, out); the rail board's, only while both boards show the same list; nothing of the media player (each
    /// gesture would reach Home Assistant twice), the yacht radar or the markets' inner steps; the clock and the cards
    /// have nothing inside.
    enum Inside: Equatable { case clicks, stops, trains, notCarried, nothing }
    static func inside(key: String) -> Inside {
        switch key {
        case "lua": return .clicks
        case "world", "flights": return .stops
        case "trains": return .trains
        case "media", "yachts", "market": return .notCarried
        default: return .nothing
        }
    }

    enum Replay: Equatable { case none, fn(String), leave }
    /// What a person's gesture on a page with a stop inside asks of the other side, which shows the same page and is
    /// in its stop or not (OTHER). ENTERED: the source was in its stop when the gesture came (the firmware takes it
    /// before it acts). A press goes in (/api/ir/do?fn=ok), or out - by a show of the page, which leaves the stop
    /// (main.cpp panelShowPage) - except on the rail board, whose press steps its stops (lists, station, out); a turn
    /// inside turns there (fn=cw|ccw); a turn outside is a page change, the screen's.
    static func replay(kind: String, entered: Bool, other: Bool, trains: Bool) -> Replay {
        switch kind {
        case "press":
            if !entered { return other ? .none : .fn("ok") }
            if trains { return other ? .fn("ok") : .none }
            return other ? .leave : .none
        case "cw", "ccw": return entered && other ? .fn(kind) : .none
        default: return .none
        }
    }

    private func gesture(_ s: Side, kind: String, key: String, name: String, entered: Bool) throws {
        let o = s.other, inside = SyncEngine.inside(key: key)
        guard inside != .nothing, kind == "press" || entered else { return }
        if inside == .notCarried {
            sayOnce("inside-\(key)", M("inside \(name): not carried (\(key == "media" ? "each gesture would reach Home Assistant twice" : "its own state on each device"))",
                                       "внутри экрана «\(name)» не синхронизируется (\(key == "media" ? "каждое действие ушло бы в Home Assistant дважды" : "у каждого устройства своё состояние"))"))
            return
        }
        guard let y = screen[o], y.key == key, y.name == name else {
            log("note", "inside", M("\(s.word.en): \(kind) inside \(name) not carried: \(o.word.en) shows another page", "\(s.word.ru): \(kind) внутри «\(name)» не переношу: \(o.on) другая страница"))
            return
        }
        if key == "flights", holdingNoAsk {
            sayOnce("inside-noask", M("inside the flight board: not carried while the twin holds ZZZZ", "внутри табло рейсов: не переношу, пока двойник держит ZZZZ")); return
        }
        let other = enteredExpect[o].flatMap { $0.until > Date() ? $0.value : nil } ?? y.entered
        let r = SyncEngine.replay(kind: kind, entered: entered, other: other, trains: inside == .trains)
        guard r != .none else { return }
        if inside == .trains {
            let a = try device(s).get("/api/railboard"), b = try device(o).get("/api/railboard")
            guard let a, let b, a.s("list") == b.s("list"), a.s("knob") == b.s("knob") else {
                log("note", "inside", M("\(name): the two boards show different lists - \(kind) not carried", "\(name): табло на сторонах показывают разное — \(kind) не переношу")); return
            }
        }
        switch r {
        case .leave: enqueue(o, .leave(key: key, name: name), gap: SyncEngine.gestureGap); enteredExpect[o] = (false, Date().addingTimeInterval(2))
        case let .fn(f):
            enqueue(o, .gesture(fn: f, key: key, name: name), gap: SyncEngine.gestureGap)
            if f == "ok", inside == .stops { enteredExpect[o] = (true, Date().addingTimeInterval(2)) }
        case .none: break
        }
    }

    /// Queued for side O, GAP after the last thing queued for it, not before AT.
    private func enqueue(_ o: Side, _ a: InsideAct, gap: TimeInterval, at wanted: Date = Date()) {
        let last = insideQ.filter { $0.to == o }.map(\.at).max() ?? insideDone[o] ?? .distantPast
        insideQ.append((o, a, max(wanted, last.addingTimeInterval(gap)), Date()))
        insideQ.sort { $0.at < $1.at }
    }
    private func queuedClicks(_ s: Side) -> Int { insideQ.filter { if $0.to == s, case .click = $0.act { return true }; return false }.count }
    private func dropClicks(_ s: Side) { insideQ.removeAll { if $0.to == s, case .click = $0.act { return true }; return false } }

    /// What is due of the queue, in order.
    private func runInside() throws {
        while let i = insideQ.firstIndex(where: { $0.at <= Date() }) {
            let q = insideQ.remove(at: i)
            try perform(q.to, q.act, since: q.since)
        }
    }

    private func perform(_ o: Side, _ a: InsideAct, since: Date) throws {
        guard let y = screen[o] else { return }
        // A datagram moved that side and the screen round has not read it since: done once it has.
        if stale.contains(o) {
            if Date().timeIntervalSince(since) < SyncEngine.openWait { insideQ.append((o, a, Date().addingTimeInterval(0.2), since)); insideQ.sort { $0.at < $1.at } }
            return
        }
        let arrow = o.other.arrow
        switch a {
        case let .click(name, run):
            guard y.key == "lua", y.name == name, !y.notify else { dropClicks(o); clicksDue = true; return }
            if let run {
                // Its effect reopened meanwhile: counted again. Its first frame not come yet: waited for, openWait at most,
                // once for its run (clickWait).
                guard var f = y.fxHere, f.run == run else { dropClicks(o); clicksDue = true; return }
                if SyncEngine.readsForOpen(open: f.open, gaveUp: fxGaveUp[o] == run, panel: o == .panel, strained: strained) {
                    let z = try readPanelOnly(o)
                    guard z.key == "lua", z.name == name, let g = z.fxHere, g.run == run else { dropClicks(o); clicksDue = true; return }
                    f = g
                }
                switch SyncEngine.clickWait(open: f.open, waited: Date().timeIntervalSince(since), gaveUp: fxGaveUp[o] == run, panel: o == .panel) {
                case .send: break
                case .drop:
                    dropClicks(o); return                                    // given up for this run already: said once
                case .giveUp:
                    // Not asked for again: the counts are taken as they are (the source's are already), and nothing more
                    // is sent to this run until it changes or opens (reconcileClicks) - no reading of it meanwhile.
                    dropClicks(o); fxGaveUp[o] = run
                    log("note", "inside", M("\(name): \(o.word.en)'s effect did not open in \(Int(SyncEngine.openWait)) s - these clicks are not carried, nor any more until it opens or reopens",
                                            "\(name): эффект \(o.of) не открылся за \(Int(SyncEngine.openWait)) с — эти нажатия не переношу, и следующие тоже, пока он не откроется или не переоткроется"))
                    return
                case let .wait(after):
                    // Under strain the panel is not read for it: its next screen round (every strainedFastEvery) tells.
                    let later = max(Date().addingTimeInterval(after), o == .panel && strained ? nextFast.addingTimeInterval(0.2) : .distantPast)
                    for k in insideQ.indices where insideQ[k].to == o { insideQ[k].at = max(insideQ[k].at, later) }
                    insideQ.append((o, a, later, since)); insideQ.sort { $0.at < $1.at }
                    return
                }
            }
            do {
                try device(o).post("/api/lua", ["click": true])
                if let run, let b = fxBase[o], b.run == run { fxBase[o]!.base += 1 }      // sync's own: no person's to carry back
                insideDone[o] = Date()
                log(arrow, "inside", M("\(name): a click", "\(name): нажатие"))
            } catch {
                // Landed or not, the answer is lost: the side's next reading is its base, and the counts are compared again.
                dropClicks(o); fxRebase.insert(o); clicksDue = true
                if error is SyncStopped || error is SyncDown || error is SyncRestart { throw error }
                log("error", "inside", M("\(name): the click to \(o.word.en) failed: " + describe(error).en, "\(name): нажатие \(o == .panel ? "на панель" : "на двойника") не прошло: " + describe(error).ru))
            }
        case let .gesture(fn, key, name):
            guard y.key == key, y.name == name else { return }
            try device(o).action("/api/ir/do", [("fn", fn)])
            insideDone[o] = Date()
            let what = fn == "ok" ? M("press (OK)", "нажатие (OK)") : fn == "cw" ? M("a turn right", "поворот вправо") : M("a turn left", "поворот влево")
            log(arrow, "inside", M("\(name): " + what.en, "\(name): " + what.ru))
        case let .leave(key, name):
            guard y.key == key, y.name == name, !y.notify else { return }
            let ans = try device(o).post("/api/panel", ["show": ["page": y.page]])
            var z = y; z.apply(panel: ans); screen[o] = z; pageReadAt[o] = Date()
            if o == .twin { sawTwin(z, at: Date()) }
            insideDone[o] = Date()
            log(arrow, "inside", M("\(name): out of its stop", "\(name): выход из страницы"))
        }
    }

    /// A side's /api/panel alone (an effect's first frame waited for); whatever moved there besides goes to the screen
    /// round, now.
    private func readPanelOnly(_ s: Side) throws -> Screen {
        guard var x = screen[s], let pn = try device(s).panelDoc(input: nil) else { throw SyncError(M("\(s.word.en): GET /api/panel failed", "\(s.word.ru): GET /api/panel не прочитан")) }
        let prev = x
        x.apply(panel: pn)
        docs[s, default: [:]]["/api/panel"] = pn; pageReadAt[s] = Date()
        if SyncEngine.renumbered(prev: prev, now: x) { shifted[s] = x.shown }
        screen[s] = x
        if s == .twin { sawTwin(x, at: Date()) }
        if x.fields != prev.fields { stale.insert(s); nextFast = Date() }
        return x
    }

    /// How many clicks to send to each side: C, a side's clicks now; B, those of them taken into account (sync's own,
    /// carried, or there before sync saw the effect); Q, those queued for it. A side's clicks above B are a person's, and
    /// the other side is given as many - a click made on each side at once counts twice. Nothing else ever goes to the
    /// panel: a click there holds its carousel and is no person's (the review of 2026-10-01, C3: the panel's effect,
    /// reopened by an upload sync carried, was given the twin's six). CATCHUP (the twin's effect alone opened anew, on the
    /// page sync put there - reconcileClicks): the twin is also brought up to the panel's count, B + Q against B + Q.
    static func clickPlan(panel p: (c: Int, b: Int, q: Int), twin t: (c: Int, b: Int, q: Int), catchUp: Bool = false) -> (toPanel: Int, toTwin: Int) {
        let pp = max(0, p.c - p.b), pt = max(0, t.c - t.b)
        return (pt, pp + (catchUp ? max(0, (p.b + p.q) - (t.b + t.q)) : 0))
    }
    enum ClickWait: Equatable { case send, wait(TimeInterval), giveUp, drop }
    /// A click queued WAITED seconds ago for a side whose effect is OPEN or not: sent once it is; else looked at again a
    /// moment later - 0.4 s on the panel (each look holds its loop()), 0.2 s on the twin - for openWait at most, and then
    /// given up for that run of the effect: the clicks queued are dropped, and so is every later one while it does not
    /// open (GAVEUP) - never asked for again round after round (the review of 2026-10-01: GET /api/panel every 0.6 s for as
    /// long as the panel's effect stayed shut, 142 requests a minute against 52).
    static func clickWait(open: Bool, waited: TimeInterval, gaveUp: Bool, panel: Bool) -> ClickWait {
        if open { return .send }
        if gaveUp { return .drop }
        if waited >= openWait { return .giveUp }
        return .wait(panel ? 0.4 : 0.2)
    }
    /// Whether a side whose effect is not open (as last read) is read for it now: not once given up for its run, and the
    /// panel not while it is under strain - its screen round every strainedFastEvery tells instead.
    static func readsForOpen(open: Bool, gaveUp: Bool, panel: Bool, strained: Bool) -> Bool {
        !open && !gaveUp && !(panel && strained)
    }
    /// The clicks of a side's effect taken into account when sync first sees its run on the page both show (a reading of
    /// the page's own effect, Screen.fxHere): MOVED - the page was put there by sync or by its carousel, not by a person
    /// there - all of them (pressed before the other side showed it, they are not carried, like any gesture on a page the
    /// other side does not show); else none - a person chose the page, or its effect reopened under them (an upload, a
    /// removal: luaEffectsReload), and every click since is theirs.
    static func newRunBase(clicks: Int, moved: Bool) -> Int { moved ? clicks : 0 }
    /// When to send N clicks from NOW: with the gaps of the person's last N presses where they are known and recent (a
    /// double press stays one), else clickGap apart - single presses, past any effect's gesture window.
    static func clickTimes(_ n: Int, presses: [Date], now: Date) -> [Date] {
        guard n > 0 else { return [] }
        let recent = Array(presses.filter { now.timeIntervalSince($0) < 10 }.suffix(n))
        var out = [now]
        for i in 1..<n {
            let gap = recent.count == n ? min(max(recent[i].timeIntervalSince(recent[i - 1]), 0.06), 1.0) : clickGap
            out.append(out[i - 1].addingTimeInterval(gap))
        }
        return out
    }

    /// Both sides on the same effect (2.7.13, now.fx): each side's clicks not yet taken into account are a person's,
    /// and the other side is given them (POST /api/lua {"click":true}, X-Twin-Sync: 1 - it holds that side's carousel
    /// as the knob's click would), once its effect is open (fx.open). Only a reading of the page's own effect counts
    /// (Screen.fxHere): right after a page change a side still tells of the effect before it, whose clicks were taken for a
    /// person's in the new one (the review of 2026-10-01, C5: each step of the carousel between two effects sent the clicks
    /// left on the one before to both sides, and each of them held the panel's carousel - 17 s a slot instead of 8).
    private func reconcileClicks() throws {
        clicksDue = false
        guard let p = screen[.panel], let t = screen[.twin], p.key == "lua", t.key == "lua", p.name == t.name, !p.name.isEmpty,
              let fp = p.fxHere, let ft = t.fxHere else { return }
        var b: [Side: Int] = [:], fresh = Set<Side>(), moved = Set<Side>()
        let c: [Side: Int] = [.panel: fp.clicks, .twin: ft.clicks]
        for (s, f) in [(Side.panel, fp), (Side.twin, ft)] {
            if let g = fxGaveUp[s], g != f.run || f.open { fxGaveUp[s] = nil }  // reopened, or open at last: clicks go again
            if fxRebase.remove(s) != nil || (fxBase[s] == nil && fxFirst[s] == "\(p.name)|\(f.run)") {
                fxBase[s] = (p.name, f.run, f.clicks)                     // taken as they are: no person's to carry
            } else if let o = fxBase[s], o.name == p.name, o.run == f.run {
            } else {
                // Opened anew: on a page sync or a carousel put there, none of its clicks so far is a person's to carry;
                // on a person's page, or reopened under them, every one (newRunBase).
                let m = movedTo[s] == p.shown
                fxBase[s] = (p.name, f.run, SyncEngine.newRunBase(clicks: f.clicks, moved: m)); dropClicks(s)
                fresh.insert(s); if m { moved.insert(s) }
            }
            if movedTo[s] == p.shown { movedTo[s] = nil }                  // its effect is taken into account now
            b[s] = fxBase[s]!.base
        }
        // The twin's effect alone opened anew, on the page sync put there (it restarted, or came back to the panel's page
        // after a person there): brought up to the panel's count. Not one reopened under a person (an upload there, which
        // sync carries next, reopening the panel's too), and never the panel.
        let catchUp = fresh == [.twin] && moved == [.twin]
        let plan = SyncEngine.clickPlan(panel: (c[.panel]!, b[.panel]!, queuedClicks(.panel)), twin: (c[.twin]!, b[.twin]!, queuedClicks(.twin)), catchUp: catchUp)
        for s in sides { fxBase[s]!.base = max(b[s]!, c[s]!) }
        let now = Date()
        for (to, n) in [(Side.twin, plan.toTwin), (Side.panel, plan.toPanel)] where n > 0 {
            let run = (to == .panel ? fp : ft).run
            if fxGaveUp[to] == run { continue }                            // its effect did not open: not sent, not read for
            if to == .twin && catchUp {
                log("panel→twin", "inside", M("\(p.name): the twin's effect opened anew - brought up to the panel's count (\(n) clicks)",
                                              "\(p.name): эффект двойника открылся заново — довожу до счёта панели (нажатий: \(n))"))
            }
            for at in SyncEngine.clickTimes(n, presses: presses[to.other] ?? [], now: now) {
                enqueue(to, .click(name: p.name, run: run), gap: 0.06, at: at)
            }
        }
    }

    /// A firmware without now.fx (before 2.7.13) on either side: the bridge of the design - while both show the same
    /// effect, each side's knob clicks (/api/knob stats click + long, read only then) that grew since the last read on
    /// that page are a person's, and the other side is given as many, clickGap apart and not before clickGap after sync
    /// wrote a page to it. Sync's own clicks (/api/lua) are not the knob's: no echo (the design's E2).
    private func bridgeClicks() throws {
        guard let p = screen[.panel], let t = screen[.twin], p.key == "lua", t.key == "lua", p.name == t.name, !p.name.isEmpty,
              p.fx == nil || t.fx == nil else { knobSeen = [:]; return }
        for s in sides {
            guard let k = try device(s).get("/api/knob"), let st = k.o("stats") else { knobSeen[s] = nil; continue }
            let n = (st.i("click") ?? 0) + (st.i("long") ?? 0), prev = knobSeen[s]
            knobSeen[s] = (p.shown, n)
            guard let prev, prev.page == p.shown, n > prev.n else { continue }
            let o = s.other
            guard screen[o]?.notify != true else { continue }
            let first = max(Date(), (lastWrite[o] ?? .distantPast).addingTimeInterval(SyncEngine.clickGap))
            for i in 0..<min(n - prev.n, 10) { enqueue(o, .click(name: p.name, run: nil), gap: SyncEngine.clickGap, at: first.addingTimeInterval(Double(i) * SyncEngine.clickGap)) }
        }
    }

    /// The end of a screen round: the gestures its reads gave, then the effects' clicks.
    private func insidePass() throws {
        for s in sides {
            let r = ringInputs[s] ?? []; ringInputs[s] = nil
            for j in r {
                let ms = (j["epochMs"] as? NSNumber)?.doubleValue ?? 0
                try inputEvent(s, j, at: ms > 0 ? Date(timeIntervalSince1970: ms / 1000) : Date())
            }
        }
        if screen[.panel]?.fx != nil && screen[.twin]?.fx != nil { try reconcileClicks() } else { try bridgeClicks() }
    }

    private func forgetInside() {
        insideQ = []; fxBase = [:]; fxFirst = [:]; fxRebase = []; clicksDue = false; presses = [:]; ringInputs = [:]; knobSeen = [:]
        movedTo = [:]; fxGaveUp = [:]
        enteredExpect = [:]; insideDone = [:]; stale = []; lastWrite = [:]
    }

    /// Each read of a side's /api/panel: where its gestures are numbered (the first read: from there on), and those the
    /// ring gave (?input=), for the end of the round; the effect it showed at the first read (its clicks then are history).
    private func noteInputs(_ s: Side, _ x: Screen) {
        if fxFirst[s] == nil { fxFirst[s] = x.fxHere.map { "\(x.name)|\($0.run)" } ?? "" }
        guard let q = x.inputSeq else { return }
        guard let last = ev[s]?.inputSeq, q >= last else { ev[s, default: Ev()].inputSeq = q; return }
        let new = x.inputs.filter { ($0.i("seq") ?? 0) > last }
        if !new.isEmpty { ringInputs[s, default: []] += new }
    }

    // MARK: effects

    /// The massive change that stops a round: nothing is written, the person is asked for the direction.
    private func massChange(_ s: Side, _ m: Msg) throws -> Never {
        log("note", "hold", M("\(m.en): nothing is carried - which way?", "\(m.ru): ничего не переношу — в какую сторону?"), problem: true)
        aligned = false; needDirection = true; question = nil; holdWhy = m
        throw SyncWait(M("waiting for your answer: \(m.en)", "жду ответа: \(m.ru)"))
    }

    private struct FxChanges { var changed: [Side: [String]] = [:], conflicts: [String] = [], deletes: [Side: Int] = [:] }

    /// Each side's effect changes against its base; a key both changed differently goes to the panel.
    private func effectChanges(_ cur: [Side: Effects]) -> FxChanges {
        var r = FxChanges()
        for s in sides {
            let b = base[s]?["effects"] ?? [:], f = cur[s]!.compared(base: b)
            if b["mode"] != f["mode"] { r.changed[s] = []; continue }         // compared another way now: rebased, not a change
            // A walk switch of an effect that just came or went is part of that change.
            r.changed[s] = Set(f.keys).union(b.keys).filter { $0 != "mode" && f[$0] != b[$0] }
                .filter { !$0.hasPrefix("w.") || (f[$0] != nil && b[$0] != nil) }.sorted()
        }
        for k in Set(r.changed[.panel]!).intersection(r.changed[.twin]!).sorted() {
            let n = String(k.dropFirst(2))
            let differ = k.hasPrefix("s.") ? !Effects.same(cur[.panel]!, cur[.twin]!, n) : cur[.panel]!.fields[k] != cur[.twin]!.fields[k]
            r.changed[.twin]!.removeAll { $0 == k }                        // no clock for these: the panel's stands
            if differ { r.conflicts.append("effect " + n) }
        }
        for s in sides { r.deletes[s] = r.changed[s]!.filter { $0.hasPrefix("s.") && cur[s]!.compared(base: base[s]?["effects"])[$0] == nil }.count }
        return r
    }

    /// GET /api/lua of both sides, and their changes carried. A script new to a list or changed in size has its SHA-256
    /// read first, when its side's schedule lets it (hashTick: one, hashGap after the last); the rest wait for their turn,
    /// not compared meanwhile (Effects.compared). Effects aligned late have every script's content read first (settle).
    private func effectsRound() throws {
        let settle = !(hasFxBase(.panel) && hasFxBase(.twin))
        var cur: [Side: Effects] = [:]
        for s in sides {
            guard var e = try readEffects(s, settle: settle) else {
                if fxAbsent[s] == true {
                    noteOnce("fx-absent-\(s)", M("the effects of \(s.word.en) cannot be read with its firmware (no /api/lua, no script store or no filesystem): not compared",
                                                 "эффекты \(s.of) не прочитать с этой прошивкой (нет /api/lua, хранилища скриптов или файловой системы): не сравниваю"))
                } else {
                    noteOnce("fx-unknown-\(s)", M("the effects of \(s.word.en) cannot be read just now (/api/lua/source did not answer): compared when they can be",
                                                  "эффекты \(s.of) сейчас не прочитать (/api/lua/source не ответил): сравню, когда прочитаются"))
                }
                return
            }
            // Compared by content from now on (a firmware that gives the scripts out): all of them read once, so the
            // base's sizes give way to the contents at once.
            if !e.unknown.isEmpty, hasFxBase(s), base[s]?["effects"]?["mode"] != "hash" { e = try readEffects(s, settle: true) ?? e }
            if !e.unknown.isEmpty, try hashTick(s, e, inRound: true) { knownNow(&e, s) }
            cur[s] = e
        }
        effects = cur
        for s in sides where !cur[s]!.byHash {
            noteOnce("fx-size-\(s)-\(fw[s]?.id ?? "")", M("the effects of \(s.word.en) are compared by size: its firmware has no /api/lua/source, so an edit that keeps a script's length is not seen",
                                                         "эффекты \(s.of) сравниваются по размеру: в прошивке \(s.of) нет /api/lua/source, и правку, не меняющую длину скрипта, не видно"))
        }
        try effectsPass(cur, allowMass: false)
        pruneRefusals()
        try renameTwinFiles()
    }

    private func effectsPass(_ cur: [Side: Effects], allowMass: Bool) throws {
        try noRestartPending()
        guard hasFxBase(.panel), hasFxBase(.twin) else { try alignEffectsLate(cur); return }
        for s in sides where base[s]!["effects"]!["mode"] != cur[s]!.fields["mode"] {
            log("note", "effects", M("the effects of \(s.word.en) are compared by \(cur[s]!.byHash ? "content" : "size") from now on", "эффекты \(s.of) теперь сравниваются \(cur[s]!.byHash ? "по содержимому" : "по размеру")"))
        }
        let ch = effectChanges(cur)
        for c in ch.conflicts { log("panel→twin", "conflict", M("conflict: the panel's taken - \(c)", "конфликт: взята панель — \(c.replacingOccurrences(of: "effect ", with: "эффект "))")) }
        if !allowMass {
            for s in sides where ch.deletes[s]! > SyncEngine.MASS_DELETES {
                try massChange(s, M("\(ch.deletes[s]!) effects removed on \(s.word.en) at once", "\(s.on) сразу удалено эффектов: \(ch.deletes[s]!)"))
            }
        }
        let old: [Side: [String: String]] = [.panel: base[.panel]!["effects"]!, .twin: base[.twin]!["effects"]!]
        // Both bases first: a write to a side reads it again and makes that its base; a failed write puts
        // its source's old base back afterwards.
        for s in sides { base[s, default: [:]]["effects"] = cur[s]!.compared(base: old[s]) }
        var failed: [(Side, [String], Error)] = []
        for s in sides where !ch.changed[s]!.isEmpty {
            let keys = ch.changed[s]!
            do { try applyEffects(from: s, keys: keys); forgive("effects", s, keys) } catch { failed.append((s, keys, error)) }
        }
        for (s, keys, e) in failed { revert("effects", s, keys, old[s]!, e) }
        if let e = failed.first(where: { $0.2 is SyncStopped })?.2 ?? failed.first?.2 { throw e }
    }

    /// The effects have no base: they were not aligned with the rest, since a side's could not be read then.
    /// Now both can be: aligned in the direction the person chose then (fxAlignFrom), unless that would
    /// remove more than MASS_DELETES on the other side - then, or when no direction was chosen for them,
    /// the direction is asked. Never both taken as they are: whatever differs would stay so for good.
    private func alignEffectsLate(_ cur: [Side: Effects]) throws {
        guard let from = fxAlignFrom else {
            try massChange(.panel, M("the effects of the panel and the twin have not been aligned", "эффекты панели и двойника ещё не выровнены"))
        }
        let to = from.other, a = cur[from]!, b = cur[to]!
        let removals = b.bytes.keys.filter { a.bytes[$0] == nil }.count
        if removals > SyncEngine.MASS_DELETES {
            fxAlignFrom = nil
            try massChange(to, M("aligning the effects \(from.arrow), as you chose, would remove \(removals) on \(to.word.en)",
                                 "выравнивание эффектов \(from.arrow), как вы выбрали, удалило бы \(to.on) эффектов: \(removals)"))
        }
        log(from.arrow, "effects", M("the effects, which could not be read at the alignment, are aligned now: \(from.word.en) as it is",
                                     "эффекты, которые при выравнивании было не прочитать, выравниваю сейчас: берётся \(from.word.ru) как есть"))
        effects = cur
        do { try convergeEffects(from: from, a, b) }
        catch let e where e is SyncStopped || e is SyncDown || e is SyncWait || e is SyncRestart { throw e }
        catch {
            // A target that keeps refusing (a full LittleFS, a clashing built-in name): three tries, then ask.
            if tryAgain("effects-late", to, ["all"]) { throw error }
            fxAlignFrom = nil
            try massChange(to, M("the effects could not be aligned \(from.arrow) three times (\(error))",
                                 "эффекты не удалось выровнять \(from.arrow) трижды (\(error))"))
        }
        forgive("effects-late", to, ["all"])
        guard let pe = effects[.panel], let te = effects[.twin] else { return }
        base[.panel, default: [:]]["effects"] = pe.fields; base[.twin, default: [:]]["effects"] = te.fields
        fxAlignFrom = nil; stateDirty = true
        try renameTwinFiles()
    }

    private func convergeEffects(from s: Side, _ a: Effects, _ b: Effects) throws {
        var keys: [String] = []
        for n in Set(a.bytes.keys).union(b.bytes.keys) where !Effects.same(a, b, n) { keys.append("s." + n) }
        for n in a.bytes.keys where b.bytes[n] != nil && (a.walk[n] ?? true) != (b.walk[n] ?? true) { keys.append("w." + n) }
        if !keys.isEmpty { try applyEffects(from: s, keys: keys.sorted()) }
    }

    /// Carries effect changes of side S to the other side: removals, then new and replaced scripts, then walk switches.
    /// A script the other side refuses (UploadRefused) is left out and said once: the rest goes on (refusedFx).
    private func applyEffects(from s: Side, keys: [String]) throws {
        let o = s.other
        guard let src = effects[s] else {
            throw SyncError(M("the effects of \(s.word.en) cannot be read now", "эффекты \(s.of) сейчас не прочитать"))
        }
        var dst = try readEffectsNow(o)
        var wrote = false
        func again() throws -> Effects { try readEffectsNow(o) }
        for k in keys where k.hasPrefix("s.") && src.bytes[String(k.dropFirst(2))] == nil {
            let name = String(k.dropFirst(2))
            pendingFx[s]?[name] = nil
            guard let stem = dst.stem[name] else { continue }
            try device(o).post("/api/lua", ["delete": stem])                  // puts the clock on (web_panel.cpp:1004-1005)
            hashes[o]?[stem] = nil; listMoved(o)
            wrote = true
            log(s.arrow, "effect", M("removed \(name)", "удалён эффект \(name)"))
        }
        if wrote { dst = try again() }                                         // a removal renumbers the list
        // A script that changed on S is not sent only when the target is known to hold the same bytes: both
        // sides' hashes equal, or both compared by size and the sizes equal. A target compared by size cannot
        // tell an edit that kept the length, so it gets the script.
        func knownSame(_ n: String) -> Bool {
            guard let a = src.bytes[n], let b = dst.bytes[n] else { return false }
            if let h = src.hash[n], let g = dst.hash[n] { return h == g }
            return !src.byHash && !dst.byHash && a == b
        }
        for k in keys where k.hasPrefix("s.") {
            let name = String(k.dropFirst(2))
            // The target's script of the same size, its content not read yet on its schedule: read now, this one only - else
            // it would be sent whether it holds the same bytes or not.
            if dst.unknown.contains(name), dst.bytes[name] == src.bytes[name], let ts = dst.stem[name] {
                if let h = try scriptHash(o, stem: ts, bytes: dst.bytes[name]!) { dst.hash[name] = h }
                dst.unknown.remove(name)
            }
            guard let bytes = src.bytes[name], let stem = src.stem[name], !knownSame(name) else { continue }
            // Refused before, and the source still holds what was refused: not sent again (said then, once).
            let content = src.fields["s." + name] ?? "\(bytes)"
            if SyncEngine.stillRefused(refusedFx[o]?[name], content: content, targetScripts: dst.bytes.count) { continue }
            guard let script = try script(from: s, name: name, stem: stem, bytes: bytes, hash: src.hash[name]) else {
                pendingFx[s, default: [:]][name] = bytes
                noteOnce("fx-\(s)-\(name)-\(bytes)", M("\(name) is not carried to \(o.word.en): \(s.word.en) does not give out its script (no /api/lua/source, or a 404 for it), and the gallery of \(repo) has no such file",
                                                        "\(name) не перенести на \(o.to): \(s.word.ru) не отдаёт его скрипт (нет /api/lua/source или 404 на него), а в галерее \(repo) такого файла нет"))
                continue
            }
            // The same effect keeps the target's file name here; a name that differs only in case is the rename's
            // (renameTwinFiles: the twin's takes the panel's, with the content it has then).
            let target = dst.stem[name] ?? stem
            do { try uploadEffect(o, stem: target, data: script) } catch let r as UploadRefused {
                refusedFx[o, default: [:]][name] = Refusal(content: content, why: r.why, room: r.room ? dst.bytes.count : nil)
                pendingFx[s]?[name] = nil; stateDirty = true
                log(s.arrow, "effect", M("\(name) is not carried: \(o.word.en) refuses it - \(r.why). It is sent again when it changes on \(s.word.en)\(r.room ? ", or when \(o.word.en) has fewer scripts" : "")",
                                         "\(name) не переносится: \(o.word.ru) его не принимает — \(r.why). Отправлю снова, когда он изменится \(s.on)\(r.room ? " или когда \(o.at) станет меньше скриптов" : "")"),
                    problem: true)
                continue
            }
            refusedFx[o]?[name] = nil
            hashes[o, default: [:]][target] = (script.count, String(sha256Hex(script).prefix(16)), Date())   // what was sent is there now
            listMoved(o)
            pendingFx[s]?[name] = nil
            wrote = true
            log(s.arrow, "effect", M("\(dst.bytes[name] == nil ? "new" : "replaced") \(name) (\(bytes) B)",
                                     "\(dst.bytes[name] == nil ? "новый" : "заменён") эффект \(name) (\(bytes) Б)"))
            dst = try again()
            if src.walk[name] == false, dst.walk[name] != false, let i = dst.names.firstIndex(of: name) {
                try device(o).post("/api/lua", ["walk": ["i": i, "on": false, "name": name] as J])
                listMoved(o)
                dst = try again()
            }
        }
        for k in keys where k.hasPrefix("w.") {
            let name = String(k.dropFirst(2))
            guard let want = src.walk[name], dst.walk[name] != nil, dst.walk[name] != want, let i = dst.names.firstIndex(of: name) else { continue }
            try device(o).post("/api/lua", ["walk": ["i": i, "on": want, "name": name] as J])   // 409 if the list moved meanwhile
            listMoved(o)
            wrote = true
            log(s.arrow, "effect", M("\(name) \(want ? "in" : "out of") the walk", "\(name) \(want ? "в обходе" : "вне обхода")"))
        }
        guard wrote else { return }
        effects[o] = try again()
        base[o, default: [:]]["effects"] = effects[o]!.compared(base: base[o]?["effects"])
        try afterWrite(o, from: s)
    }

    private func uploadEffect(_ o: Side, stem: String, data: Data) throws {
        for attempt in 1...4 {
            // The upload runs a trial of the script inside loop(): up to 15 s (lua_effects.cpp:572-600).
            let a = try device(o).upload("/api/lua/upload", query: [("name", stem)], field: "script", filename: "\(stem).lua", data: data, timeout: 90)
            if a.status == 200, a.object?.b("success") == true { return }
            if a.why.contains("another upload"), attempt < 4 { Thread.sleep(forTimeInterval: 2); continue }
            if let r = SyncEngine.uploadRefusal(status: a.status, why: a.why) { throw r }
            throw device(o).refused("POST /api/lua/upload?name=\(stem)", a)
        }
    }

    /// Whether an upload's answer refuses the script for good (UploadRefused) or only this time (nil: an error as
    /// ever). Every refusal of the upload is a 400 (handleLuaUploadDone; 403 only for a foreign origin); these of
    /// its words are the transfer's or the filesystem's, not the script's, and are tried again as before
    /// (web_panel.cpp handleLuaUploadChunk, lua_store.cpp luaStoreBegin/Finish, firmware 2.7.13).
    static let uploadTransient = ["another upload", "cut short", "carried no file", "nothing was uploaded", "could not",
                                  "would not take the write", "no filesystem", "no PSRAM"]
    static let uploadNoRoom = ["uploaded slots are used", "filesystem is full", "does not fit"]
    static func uploadRefusal(status: Int, why: String) -> UploadRefused? {
        guard status == 400, !uploadTransient.contains(where: { why.contains($0) }) else { return nil }
        return UploadRefused(why: why, room: uploadNoRoom.contains { why.contains($0) })
    }

    /// The script's bytes: from the device itself where it has /api/lua/source, else the gallery file with
    /// the same name and size (and SHA-256, where the index gives one); nil when neither has it.
    private func script(from s: Side, name: String, stem: String, bytes: Int, hash: String?) throws -> Data? {
        switch luaCap(s) {
        case true?:
            // By its file's name on S, exactly; one S lists and does not give out (NoSuchScript) is not asked for again.
            guard noSource[s]?[stem] != bytes else { break }
            do {
                let d = try device(s).luaSource(stem)
                hashes[s, default: [:]][stem] = (d.count, String(sha256Hex(d).prefix(16)), Date())     // read now: known
                guard d.count == bytes, hash == nil || sha256Hex(d).hasPrefix(hash!) else {
                    throw SyncError(M("\(name) changed while it was read: carried next time", "\(name) изменился, пока читался: перенесу в следующий раз"))
                }
                return d
            } catch is NoSuchScript { noSource[s, default: [:]][stem] = bytes }
        case nil:
            throw SyncError(M("whether \(s.word.en) gives out its scripts is not known now: tried again", "отдаёт ли \(s.word.ru) скрипты, сейчас не узнать: повторю"))
        case false?:
            break
        }
        guard let g = galleryList(),
              let e = g.first(where: { $0.i("bytes") == bytes && (($0.s("stem") ?? "").lowercased() == stem.lowercased() || $0.s("name") == name) }),
              let file = e.s("file"), file.range(of: #"^[A-Za-z0-9_.-]+\.lua$"#, options: .regularExpression) != nil else { return nil }
        let d = try fetch("https://raw.githubusercontent.com/\(repo)/main/gallery/\(file)", limit: 1 << 20)
        if let want = e.s("sha256")?.lowercased(), sha256Hex(d) != want { return nil }
        return d.count == bytes ? d : nil
    }

    private func galleryList() -> [J]? {
        if let g = gallery, g.repo == repo, Date().timeIntervalSince(g.at) < 600 { return g.list }
        guard let d = try? fetch("https://raw.githubusercontent.com/\(repo)/main/gallery/index.json", limit: 4 << 20),
              let o = try? JSONSerialization.jsonObject(with: d) as? J else { return nil }
        gallery = (Date(), repo, o.a("effects"))
        return gallery!.list
    }

    /// Whether side S gives out its scripts (GET /api/lua/source): asked once per firmware; nil while not known.
    private func luaCap(_ s: Side) -> Bool? {
        let id = fw[s]?.id ?? ""
        var c = caps[s]?.id == id ? caps[s]! : Caps(id: id)
        if c.lua == nil { c.lua = device(s).probeLuaSource() }
        caps[s] = c
        return c.lua
    }
    /// Whether side S gives out its firmware image, and the ELF SHA-256 of its build; nil while not known.
    private func imageCap(_ s: Side) -> (has: Bool, elf: String?)? {
        let id = fw[s]?.id ?? ""
        var c = caps[s]?.id == id ? caps[s]! : Caps(id: id)
        if c.image == nil, let r = device(s).probeImage() { c.image = r.has; c.elf = r.elf }
        caps[s] = c
        return c.image.map { ($0, c.elf) }
    }

    /// A script a side refused (UploadRefused): the source's content then (its "s." field: "h<sha256>" or the size),
    /// the side's words, and for a refusal for want of room the number of scripts the side had.
    struct Refusal: Codable, Equatable { var content: String, why: String, room: Int? }
    /// Whether a refusal still stands (the owner, 2026-09-30 20:45: autumn_dawn, "too slow… 4 of 4 frames over 500 ms",
    /// failed the whole alignment and the direction was asked four times): the source holds the content refused, and
    /// a side refused for room has no fewer scripts than it had. Sent again only when that changes.
    static func stillRefused(_ r: Refusal?, content: String, targetScripts: Int) -> Bool {
        guard let r, r.content == content else { return false }
        return r.room.map { targetScripts >= $0 } ?? true
    }
    /// The refusals that no longer stand: the source holds other content now, or the target the same.
    private func pruneRefusals() {
        for o in sides {
            guard let r = refusedFx[o], let src = effects[o.other], let dst = effects[o] else { continue }
            // One whose content is not read yet on either side stands until it is.
            let keep = r.filter { n, x in src.unknown.contains(n) || dst.unknown.contains(n) || (src.fields["s." + n] == x.content && !Effects.same(src, dst, n)) }
            if keep.count != r.count { refusedFx[o] = keep; stateDirty = true }
        }
    }
    /// "AUTUMN DAWN to the twin: too slow…": what the direction question shows as not carried.
    private func notCarried() -> [Msg] {
        var out: [Msg] = []
        for o in sides {
            guard let src = effects[o.other] else { continue }
            for (n, r) in (refusedFx[o] ?? [:]).sorted(by: { $0.key < $1.key }) where src.fields["s." + n] == r.content {
                out.append(M("\(n) to \(o.word.en): \(r.why)", "\(n) на \(o.to): \(r.why)"))
            }
        }
        return out
    }

    /// An effect is one on both sides by the name its banner shows - FLOW for flow.lua and FLOW.lua alike: a device
    /// takes no two scripts whose names read the same ("an effect called FLOW is already on the panel",
    /// web_panel.cpp handleLuaUploadChunk) - and its file's name, GET /api/lua's uploaded.scripts[].name, is
    /// compared exactly. Where the two differ only in case, the twin's file takes the panel's name (the owner,
    /// 2026-09-30 20:45: FLOW, KALEIDOSCOPE, NEBULA on the panel, flow, kaleidoscope, nebula on the twin, the same
    /// bytes - and the list is in the files' order, so WARP was page 29 on one and 26 on the other). The panel's
    /// never: the panel leads.
    static func caseRenames(panel p: Effects, twin t: Effects) -> [(name: String, from: String, to: String)] {
        p.stem.compactMap { n, ps -> (name: String, from: String, to: String)? in
            guard let ts = t.stem[n], ts != ps else { return nil }
            return (name: n, from: ts, to: ps)
        }.sorted { $0.name < $1.name }
    }

    /// The twin's files named as the panel's but for case take the panel's names (caseRenames): each is removed and
    /// uploaded again under the panel's name with its own bytes - its content stays the twin's; a difference in it is
    /// the content's, carried as ever - and its walk switch is put back (a removal forgets it, panelEffectsPrune).
    /// The upload refused: the old file back under its old name. The page it moved (a removal shows the clock,
    /// web_panel.cpp handleLua) is put back as after any write (afterWrite).
    private func renameTwinFiles() throws {
        guard let p = effects[.panel], let t = effects[.twin] else { return }
        let todo = SyncEngine.caseRenames(panel: p, twin: t)
        guard !todo.isEmpty else { return }
        try noRestartPending()
        var cur = t, wrote = false, did: [String] = []
        for r in todo {
            guard let bytes = cur.bytes[r.name], cur.stem[r.name] == r.from else { continue }
            if let c = renameRefused[r.name], c == cur.fields["s." + r.name] { continue }        // said when refused
            guard let data = try script(from: .twin, name: r.name, stem: r.from, bytes: bytes, hash: cur.hash[r.name]) else {
                noteOnce("rename-\(r.from)-\(bytes)", M("\(r.name): the twin's file \(r.from) is named as the panel's \(r.to) but for case, and its bytes cannot be read to upload them again: left as it is",
                                                         "\(r.name): файл двойника \(r.from) отличается от панельного \(r.to) только регистром, но его байты не прочитать, чтобы загрузить заново: оставляю как есть"))
                continue
            }
            let walkOff = cur.walk[r.name] == false
            try device(.twin).post("/api/lua", ["delete": r.from])
            hashes[.twin]?[r.from] = nil; hashes[.twin]?[r.to] = nil; wrote = true; listMoved(.twin)
            did.append(r.name)
            let known = (bytes: data.count, hash: String(sha256Hex(data).prefix(16)), at: Date())   // its own bytes, under either name
            do {
                try uploadEffect(.twin, stem: r.to, data: data)
                hashes[.twin, default: [:]][r.to] = known
                log("panel→twin", "effect", M("\(r.name): the twin's file \(r.from) is named \(r.to) now, as on the panel", "\(r.name): файл двойника \(r.from) теперь называется \(r.to), как на панели"))
            } catch let e as UploadRefused {
                renameRefused[r.name] = cur.fields["s." + r.name]
                let back = (try? uploadEffect(.twin, stem: r.from, data: data)) != nil
                if back { hashes[.twin, default: [:]][r.from] = known }
                log("panel→twin", "effect", M("\(r.name): the twin's file \(r.from) was not renamed \(r.to) - the twin refused the upload (\(e.why)); \(back ? "its old name is back" : "its old name could not be put back either: it is carried from the panel as a new effect")",
                                              "\(r.name): файл двойника \(r.from) не переименован в \(r.to) — двойник не принял загрузку (\(e.why)); \(back ? "прежнее имя возвращено" : "прежнее имя тоже вернуть не удалось: перенесу его с панели как новый эффект")"),
                    problem: true)
            }
            cur = try readEffectsNow(.twin)
            if walkOff, cur.bytes[r.name] != nil, cur.walk[r.name] != false, let i = cur.names.firstIndex(of: r.name) {
                try device(.twin).post("/api/lua", ["walk": ["i": i, "on": false, "name": r.name] as J])
                listMoved(.twin)
                cur = try readEffectsNow(.twin)
            }
        }
        guard wrote else { return }
        effects[.twin] = cur
        // The renamed effects' base only: their content and walk are what they were; anything else is the next pass's.
        // One the twin lost (refused, and not put back) is not the twin's removal: the panel's goes to it as new.
        if base[.twin]?["effects"] != nil {
            for n in did {
                for k in ["s." + n, "w." + n] { base[.twin]!["effects"]![k] = cur.fields[k] }
                if cur.bytes[n] == nil { base[.panel]?["effects"]?["s." + n] = nil }
            }
        }
        stateDirty = true
        try afterWrite(.twin, from: .panel)
    }

    /// Effects that could not be carried: tried again when the source side's firmware or the repository changed.
    private func retryPending() {
        for s in sides {
            guard let p = pendingFx[s], !p.isEmpty else { continue }
            noted = noted.filter { !$0.hasPrefix("fx-\(s)-") }
            if let e = effects[s] { try? applyEffects(from: s, keys: p.keys.filter { e.bytes[$0] == p[$0] }.map { "s." + $0 }) }
        }
    }

    // MARK: settings and firmware

    private func slowRound(market: Bool) throws {
        let strained = strainedUntil.map { Date() < $0 } ?? false
        dev[.panel]?.pace = strained ? SyncEngine.strainedPace : Device.panelPace
        defer { dev[.panel]?.pace = Device.panelPace }
        for s in sides { try readDocs(s, SyncEngine.settingsRoutes + (market ? ["/api/market"] : [])) }
        // Who is who, again: an address can change hands.
        if fw[.panel]!.mac != panelMac || fw[.twin]!.mac != twinMac || !fw[.twin]!.name.hasPrefix("TWIN-") {
            reconnect(); throw SyncWait(M("the devices changed: connecting again", "устройства сменились: подключаюсь заново"))
        }
        nextVerify = Date().addingTimeInterval(SyncEngine.verifyEvery)
        watchLoad()
        firmwareRound()
        try settingsPass(allowMass: false)
        walkHeld = false
    }

    /// The panel's own counters of refused requests and failed allocations (web.cpp:477-485): when they grow,
    /// sync polls it less.
    private func watchLoad() {
        guard let f = fw[.panel] else { return }
        defer { load = (f.uptime, f.webRefused, f.allocFails) }
        guard let l = load, f.uptime >= l.uptime else { return }             // a reboot starts the counters again
        if f.webRefused > l.refused || f.allocFails > l.fails {
            if strainedUntil.map({ Date() >= $0 }) ?? true {
                log("note", "load", M("the panel is under strain (refused requests \(l.refused)→\(f.webRefused), failed allocations \(l.fails)→\(f.allocFails)): polling it less for 10 min",
                                      "панель под нагрузкой (отказы \(l.refused)→\(f.webRefused), неудачные выделения памяти \(l.fails)→\(f.allocFails)): 10 мин опрашиваю её реже"))
            }
            strainedUntil = Date().addingTimeInterval(SyncEngine.strainFor)
        }
    }

    /// What each side changed since its base, as settingsPass carries it: a key both changed goes from the
    /// panel (CONFLICTS: the ones whose values differ), the panel's hardware never from the twin (HARDWARE),
    /// a key new on a side (a newer firmware, a module that appeared) is nobody's change - except what the
    /// owner's old overrides left on the twin (Settings.residue): with no twin base yet, the panel's value goes
    /// to the twin (RESIDUE, among the panel's). LOST: the twin restarted since the last full pass, and these
    /// keys are writes of sync's it lost - each module saves its settings to NVS a moment later (SETTLE_MS 2.5 s,
    /// lostWithin), and a restart within that moment keeps the old value, which the twin shows again (lost): the
    /// panel's value goes back to the twin (RESTORED, among the panel's). What else differs on the twin after a
    /// restart is its own change, carried as ever - a person's choice it had saved before the restart. ONLY:
    /// these keys alone.
    static func settingsChanges(cur: [Side: J], dig: [Side: [String: String]], base bp: [String: String], _ bt: [String: String],
                                only: ((String) -> Bool)? = nil, lost: Set<String> = [])
        -> (changed: [Side: [String]], conflicts: [String], hardware: [String], residue: [String], restored: [String]) {
        var changed: [Side: [String]] = [:]
        for (s, b) in [(Side.panel, bp), (.twin, bt)] {
            changed[s] = dig[s]!.keys.filter { (only?($0) ?? true) && b[$0] != nil && b[$0] != dig[s]![$0] }.sorted()
        }
        let lostNow = changed[.twin]!.filter { lost.contains($0) }
        let restored = lostNow.filter { !changed[.panel]!.contains($0) }
        changed[.panel] = Array(Set(changed[.panel]!).union(lostNow)).sorted()
        changed[.twin]!.removeAll { lost.contains($0) }
        let residue = dig[.panel]!.keys.filter { k in
            (only?(k) ?? true) && bt[k] == nil && cur[.twin]![k] != nil && Settings.residue(k, twin: cur[.twin]![k])
                && canon(cur[.panel]![k]) != canon(cur[.twin]![k])
        }.sorted()
        changed[.panel] = Array(Set(changed[.panel]!).union(residue)).sorted()
        var conflicts: [String] = []
        for k in Set(changed[.panel]!).intersection(changed[.twin]!).sorted() {
            changed[.twin]!.removeAll { $0 == k }
            if canon(cur[.panel]![k]) != canon(cur[.twin]![k]) { conflicts.append(k) }
        }
        let hardware = changed[.twin]!.filter { Settings.hardware.contains($0) }
        changed[.twin]!.removeAll { Settings.hardware.contains($0) }
        return (changed, conflicts, hardware, residue, restored)
    }

    /// The settings each side changed since its base, carried to the other side. ONLY: these keys alone
    /// (the carousel's, in every screen round: Settings.walkKey), the others' bases left as they are.
    private func settingsPass(allowMass: Bool, only: ((String) -> Bool)? = nil) throws {
        let cur: [Side: J] = [.panel: flat(.panel), .twin: flat(.twin)]
        let dig = cur.mapValues(digests)
        guard let bp = base[.panel]?["settings"], let bt = base[.twin]?["settings"], !bp.isEmpty, !bt.isEmpty else {
            if only != nil { return }                               // no bases yet: the settings round sets them
            for s in sides { base[s, default: [:]]["settings"] = dig[s]! }
            try enforceOverrides(); return
        }
        try noRestartPending()
        // The twin's restart is judged by the first full pass after it (walkHeld keeps the screen round's
        // carousel pass waiting for it): of the writes it may have lost, those it shows again as they were.
        let lost = only == nil ? SyncEngine.lost(twinLost, now: dig[.twin]!) : []
        if only == nil { twinLost = nil }
        let ch = SyncEngine.settingsChanges(cur: cur, dig: dig, base: bp, bt, only: only, lost: lost)
        let changed = ch.changed
        for k in ch.conflicts { log("panel→twin", "conflict", M("conflict: the panel's taken - \(Settings.shown(k))", "конфликт: взята панель — \(Settings.shown(k))")) }
        // The panel's hardware never goes to the panel: the twin keeps its own until the panel changes it.
        for k in ch.hardware {
            noteOnce("hw-\(k)-\(dig[.twin]![k] ?? "")", M("\(Settings.shown(k)): the panel's hardware setting - not carried from the twin to the panel",
                                                          "\(Settings.shown(k)): настройка железа панели — с двойника на панель не переношу"))
        }
        if !ch.residue.isEmpty {
            log("override", "twin", M("no longer held on the twin (the overrides of 2026-09-29, dropped 2026-09-30 19:35; ZZZZ while the twin had no key or fbAskHa): the panel's \(ch.residue.map(Settings.shown).joined(separator: ", "))",
                                      "двойник больше не удерживает своё (переопределения 29.09, снятые 30.09 19:35; ZZZZ, пока у двойника не было ключа или fbAskHa): беру с панели \(ch.residue.map(Settings.shown).joined(separator: ", "))"))
        }
        if !allowMass {
            // Each side's own: the twin's lost writes are the twin's (an erased flash is many at once).
            let own: [Side: Int] = [.panel: changed[.panel]!.filter { !ch.residue.contains($0) && !ch.restored.contains($0) }.count,
                                    .twin: changed[.twin]!.count + ch.restored.count]
            for s in sides where own[s]! > SyncEngine.MASS_SETTINGS {
                try massChange(s, M("\(own[s]!) settings changed on \(s.word.en) at once", "\(s.on) сразу изменилось настроек: \(own[s]!)"))
            }
        }
        if !ch.restored.isEmpty {
            log("panel→twin", "restart", M("the twin restarted before it saved \(ch.restored.map(Settings.shown).joined(separator: ", ")): the panel's value goes back to it",
                                          "двойник перезагрузился, не успев сохранить \(ch.restored.map(Settings.shown).joined(separator: ", ")): возвращаю значение панели"))
        }
        for s in sides {
            if let only { for (k, v) in dig[s]! where only(k) { base[s, default: [:]]["settings", default: [:]][k] = v } }
            else { base[s, default: [:]]["settings"] = dig[s]! }
        }
        var failed: [(Side, [String], Error)] = []
        for s in sides {
            let o = s.other
            var keys: [String] = []
            for k in changed[s]! {
                guard cur[o]![k] != nil else {
                    noteOnce("key-\(o)-\(k)", M("\(k): \(o.word.en)'s firmware has no such setting", "\(k): в прошивке \(o.of) такой настройки нет")); continue
                }
                if canon(cur[o]![k]) != canon(cur[s]![k]) { keys.append(k) }
            }
            guard !keys.isEmpty else { continue }
            do { try applySettings(from: s, keys: keys, only: only); forgive("settings", s, keys) } catch { failed.append((s, keys, error)) }
        }
        for (s, keys, e) in failed {
            revert("settings", s, keys, s == .panel ? bp : bt, e)
            // What an old override left, not written because a device did not answer or sync stopped: the twin's
            // base drops it again, so it is seen again.
            if s == .panel, e is SyncDown || e is SyncStopped || e is SyncWait {
                for k in keys where ch.residue.contains(k) { base[.twin]?["settings"]?[k] = nil }
            }
        }
        if let e = failed.first(where: { $0.2 is SyncStopped })?.2 ?? failed.first?.2 { throw e }
        if only == nil { try enforceOverrides() }                  // the settings round's: it reads the twin's documents again
    }

    /// Writes the settings KEYS of side S to the other side, then reads what it wrote again as the other side's
    /// base - of the keys ONLY allows, when given (a screen round's carousel settings: the rest of the other
    /// side's base waits for the settings round, which compares it).
    private func applySettings(from s: Side, keys all: [String], only: ((String) -> Bool)? = nil) throws {
        let o = s.other, dv = device(o)
        let keys = o == .panel ? all.filter { !Settings.hardware.contains($0) } : all      // never the panel's hardware to it
        let src = flat(s), sd = docs[s] ?? [:]
        let twinBefore = o == .twin ? digests(flat(.twin)) : [:]                  // what a restart that loses the write shows
        let sForm = sd["/api/portal"]?.o("form") ?? [:], sExport = sd["/api/export"] ?? [:]
        var saveKeys = keys.filter { $0.hasPrefix("f.") }
        var body: J = [:]
        for k in keys where k.hasPrefix("x.") { body[String(k.dropFirst(2))] = src[k] }
        var region: Int?
        if keys.contains("tz") {
            let r = sForm.i("timezoneRegion") ?? -1
            if r >= 0 { region = r; saveKeys.append("tz") }                  // /save sets a region (web.cpp:1554-1568)
            else { for k in ["timezoneString", "gmtOffset", "daylightSaving"] { body[k] = sExport[k] } }   // a zone of its own: /api/import
        }
        var done: [String] = []
        if !saveKeys.isEmpty {
            // The form is the target's own, read just now, with the changed fields replaced: its hardware
            // fields ride along as they are.
            try readDocs(o, ["/api/portal", "/api/export"])
            let od = docs[o]!
            var form = od["/api/portal"]?.o("form") ?? [:]
            for k in saveKeys where k.hasPrefix("f.") { let f = String(k.dropFirst(2)); form[f] = sForm[f] }
            if let region { form["timezoneRegion"] = region }
            var pairs: [(String, String)] = []
            for (k, v) in form.sorted(by: { $0.key < $1.key }) {
                if Settings.formIdentity.contains(k) || k == "weatherApiKey" || v is NSNull { continue }
                if isBool(v) { if (v as! NSNumber).boolValue { pairs.append((k, "1")) } }     // an absent checkbox is off
                else { pairs.append((k, formText(v))) }
            }
            let oExport = od["/api/export"] ?? [:]
            for (arr, field) in Settings.metricFields {
                for (i, v) in (oExport[arr] as? [Any] ?? []).prefix(20).enumerated() { pairs.append(("\(field)\(i + 1)", formText(v))) }
            }
            for (slot, c) in (od["/api/portal"]?["spriteColors"] as? [Any] ?? []).enumerated() { pairs.append(("color_\(slot)", formText(c))) }
            try dv.save(pairs)
            done += saveKeys.map { $0 == "tz" ? "timezoneRegion" : String($0.dropFirst(2)) }
        }
        if !body.isEmpty {
            try dv.post("/api/import", body)
            done += body.keys.sorted()
        }
        for k in keys where k.hasPrefix("pages.") {
            try dv.post("/api/panel", ["enable": ["key": String(k.dropFirst(6)), "on": src[k] ?? true] as J]); done.append(k)
        }
        if keys.contains("carousel"), let c = src["carousel"] { try dv.post("/api/panel", ["carousel": c]); done.append("carousel") }
        if keys.contains("knob"), let kn = src["knob"] as? J { try dv.post("/api/knob", kn); done.append("knob") }
        if keys.contains(where: { $0.hasPrefix("worldclock.") }) { done += try worldClock(from: s, keys: keys) }
        for k in ["railboard.crs", "railboard.favourites"] where keys.contains(k) {
            let f = k == "railboard.crs" ? "crs" : "favourites"
            try dv.post("/api/railboard", [f: src[k] ?? NSNull()]); done.append(k)
        }
        if keys.contains("railboard.config") {
            if let cfg = src["railboard.config"] as? J { try dv.post("/api/railboard", ["config": cfg]); done.append("railboard.config") }
            else { noteOnce("rb-config", M("the rail board's settings follow Home Assistant on one side: not copied", "настройки табло поездов на одной стороне берутся из Home Assistant: не копирую")) }
        }
        if keys.contains(where: { $0.hasPrefix("flightboard.") }) { done += try flightBoard(from: s, keys: keys) }
        var chunk: J = [:]
        let market = keys.filter { $0.hasPrefix("market.") }
        for k in market {
            let key = String(k.dropFirst(7))
            var trial = chunk; trial[key] = src[k]
            if !chunk.isEmpty, (try jsonData(["config": trial])).count > 8192 { try dv.post("/api/market", ["config": chunk]); trial = [key: src[k] as Any] }
            chunk = trial
        }
        if !chunk.isEmpty { try dv.post("/api/market", ["config": chunk]) }
        done += market                                                     // the keys only: the values can be money
        if keys.contains("media.selected"), let sel = src["media.selected"] as? String, !sel.isEmpty {
            try dv.post("/api/media", ["select": sel]); done.append("media.selected")
        }
        for k in keys where k.hasPrefix("ir.") {
            guard let v = src[k] as? [Any], let fn = v.first as? String else { continue }
            var p = [("btn", String(k.split(separator: ".")[1])), ("fn", fn)]
            if fn == "page" {
                // The page by its key and name, looked up among the target's pages.
                let want = v.count > 1 ? v[1] as? String ?? "" : ""
                let pages = docs[o]?["/api/panel"]?.a("pages") ?? []
                guard let i = pages.first(where: { "\($0.s("key") ?? "")/\($0.s("name") ?? "")" == want })?.i("i") else {
                    noteOnce("ir-page-\(o)-\(want)", M("\(k): \(o.word.en) has no page \(want) for the remote's button", "\(k): \(o.on) нет страницы \(want) для кнопки пульта")); continue
                }
                p.append(("page", "\(i)"))
            }
            try dv.action("/api/ir/fn", p); done.append(k)
        }
        if keys.contains("logOn") { try dv.action("/api/log", [("on", (src["logOn"] as? NSNumber)?.boolValue == true ? "1" : "0")]); done.append("logOn") }
        guard !done.isEmpty else { return }
        if o == .twin { let at = Date(); for k in keys { twinWrote[k] = (twinBefore[k] ?? "", at) } }
        // Read back what was written: the target's new values become its base, so they are not sent back.
        let routes = Array(Set(keys.flatMap(Settings.routes))).filter { Device.reads.contains($0) }.sorted()
        try readDocs(o, routes)
        if o == .twin { try enforceOverrides() }
        if let only { for (k, v) in digests(flat(o)) where only(k) { base[o, default: [:]]["settings", default: [:]][k] = v } }
        else { base[o, default: [:]]["settings"] = digests(flat(o)) }
        log(s.arrow, "settings", M("settings: " + done.joined(separator: ", "), "настройки: " + done.joined(separator: ", ")))
        try afterWrite(o, from: s)
    }

    /// World clock: custom cities and home (sync.py plan_worldclock).
    private func worldClock(from s: Side, keys: [String]) throws -> [String] {
        let o = s.other, dv = device(o), src = flat(s)
        let sd = docs[s]?["/api/worldclock"] ?? [:]
        guard var od = try dv.get("/api/worldclock") else { return [] }
        let home = src["worldclock.home"] as? String
        var done: [String] = [], addedHome = false
        if keys.contains("worldclock.cities") {
            let want = Set(sd.a("cities").filter { $0.s("kind") == "custom" }.map { canon(Settings.cityKey($0)) })
            let have = Set(od.a("cities").filter { $0.s("kind") == "custom" }.map { canon(Settings.cityKey($0)) })
            for c in od.a("cities") where c.s("kind") == "custom" && !want.contains(canon(Settings.cityKey(c))) {
                try dv.post("/api/worldclock", ["remove": c["id"] ?? -1])
            }
            for c in sd.a("cities").sorted(by: { ($0.i("id") ?? 0) < ($1.i("id") ?? 0) }) where c.s("kind") == "custom" && !have.contains(canon(Settings.cityKey(c))) {
                var add: J = Settings.pick(c, ["name", "lat", "lon", "tz"])
                if c.s("name") == home { add["home"] = true; addedHome = true }
                try dv.post("/api/worldclock", ["add": add])
            }
            done.append("worldclock.cities")
            od = try dv.get("/api/worldclock") ?? od
        }
        if keys.contains("worldclock.home") && !addedHome {
            if home == nil { try dv.post("/api/worldclock", ["auto": true]) }
            else if let c = od.a("cities").first(where: { $0.s("name") == home }) { try dv.post("/api/worldclock", ["home": c["id"] ?? -1]) }
            else { noteOnce("wc-home-\(home!)", M("\(o.word.en) has no city \(home!) to make home", "\(o.at) нет города \(home!), чтобы сделать его домашним")); return done }
            done.append("worldclock.home")
        }
        return done
    }

    /// Flight board: custom airports (added, never removed: a removal resubscribes the board, panel.cpp:576-585),
    /// the AeroAPI budget, tracked flights (sync.py plan_flightboard), and the airport selected with the half it
    /// shows - by the airport's ICAO code, looked up among the target's once the new ones are added (an id is a
    /// slot on each device): POST {"airport":id,"dir":...} (web_panel.cpp:517-527, 586). Mirrored both ways
    /// since the owner dropped the override that kept the twin on ZZZZ (2026-09-30 19:35).
    private func flightBoard(from s: Side, keys: [String]) throws -> [String] {
        let o = s.other, dv = device(o), src = flat(s)
        guard let od = try dv.get("/api/flightboard") else { return [] }
        var done: [String] = []
        if keys.contains("flightboard.custom") {
            let have = Set(od.a("airports").filter { $0.s("kind") == "custom" }.compactMap { $0.s("code") })
            for a in src["flightboard.custom"] as? [[String]] ?? [] where a.count == 4 && !have.contains(a[0]) && a[0] != Settings.noAsk.icao {
                try dv.post("/api/flightboard", ["add": ["icao": a[0], "iata": a[1], "name": a[2], "tz": a[3]]])
            }
            done.append("flightboard.custom")
        }
        if keys.contains("flightboard.budget"), let b = src["flightboard.budget"] as? J { try dv.post("/api/flightboard", ["budget": b]); done.append("flightboard.budget") }
        if keys.contains("flightboard.tracked") {
            let want = Set(src["flightboard.tracked"] as? [String] ?? [])
            let have = Set(od.a("tracked").compactMap { $0.s("ident") })
            for f in have.subtracting(want).sorted() { try dv.post("/api/flightboard", ["untrack": f]) }
            for f in want.subtracting(have).sorted() { try dv.post("/api/flightboard", ["track": f]) }
            done.append("flightboard.tracked")
        }
        if keys.contains("flightboard.selection"), let sel = src["flightboard.selection"] as? [Any], let code = sel.first as? String {
            let now = keys.contains("flightboard.custom") ? (try dv.get("/api/flightboard") ?? od) : od
            if let id = now.a("airports").first(where: { $0.s("code") == code })?.i("id") {
                var body: J = ["airport": id]
                if sel.count > 1, let dir = sel[1] as? String { body["dir"] = dir }
                try dv.post("/api/flightboard", body); done.append("flightboard.selection")
            } else {
                noteOnce("fb-sel-\(o)-\(code)", M("\(o.word.en) has no airport \(code) to select on the flight board", "\(o.at) нет аэропорта \(code), чтобы выбрать его на табло рейсов"))
            }
        }
        return done
    }

    /// The owner's overrides on the twin (sync.py OVERRIDES), put back whenever they drift, never compared or
    /// sent to the panel: climateHa off (2026-09-29), and fbAskHa off (2026-09-30 22:40, firmware 2.7.13) - a
    /// twin without an AeroAPI key of its own never asks Home Assistant for a flight board, since each ask is a
    /// fetch HA pays for with the owner's key (fb_mqtt.cpp:24-45, 103-136). A twin whose firmware has no
    /// fbAskHa yet and would ask is said once (asksHa). And what the dropped overrides of 2026-09-29 added to the
    /// twin: the custom airport ZZZZ "NO REQUESTS", removed once the twin's board is on another airport (the
    /// panel's, settingsPass) - unless the panel has such an airport itself, or its list is not known. None
    /// touches a compared setting (ZZZZ is left out of flightboard.custom), so no base moves.
    private func enforceOverrides() throws {
        let tw = device(.twin), d = docs[.twin] ?? [:]
        var did: [String] = [], routes: [String] = []
        if d["/api/export"]?.b("climateHa") == true {
            try tw.post("/api/import", ["climateHa": false]); did.append("climateHa false")    // climate.cpp:127-131, 165-182
            routes += ["/api/export", "/api/portal"]
        }
        if d["/api/export"]?.b("fbAskHa") == true {
            try tw.post("/api/import", ["fbAskHa": false]); did.append("fbAskHa false")        // 2.7.13: web.cpp handleImportConfig
            if !routes.contains("/api/export") { routes.append("/api/export") }
        }
        if SyncEngine.holdsNoAsk(export: d["/api/export"], board: d["/api/flightboard"]), let fb = d["/api/flightboard"] {
            // A firmware without fbAskHa cannot be told not to ask; a custom airport is never asked for (fb_mqtt.cpp
            // mayAsk): the twin's board stays on ZZZZ "NO REQUESTS" until it has a key of its own or fbAskHa.
            noteOnce("noask-hold-\(fw[.twin]?.id ?? "")", M("the twin has an MQTT broker and no AeroAPI key, and its firmware \(fw[.twin]?.version ?? "") has no fbAskHa (2.7.13): its flight board is held on \(Settings.noAsk.icao) \"\(Settings.noAsk.name)\", which Home Assistant is never asked for, and the panel's airport is not carried to it until it has a key of its own or a firmware with fbAskHa",
                                                            "у двойника задан брокер MQTT и нет ключа AeroAPI, а в его прошивке \(fw[.twin]?.version ?? "") нет fbAskHa (2.7.13): табло рейсов двойника держу на \(Settings.noAsk.icao) «\(Settings.noAsk.name)» — его у Home Assistant не запрашивают, — а аэропорт панели не переношу, пока у двойника нет своего ключа или прошивки с fbAskHa"))
            if let body = SyncEngine.noAskHold(board: fb, panelBoard: docs[.panel]?["/api/flightboard"]) {
                do {
                    try tw.post("/api/flightboard", body); did.append("\(Settings.noAsk.icao) \"\(Settings.noAsk.name)\" selected")
                    routes.append("/api/flightboard")
                } catch let e where e is SyncStopped || e is SyncDown || e is SyncRestart { throw e }
                catch { noteOnce("noask-fail-\(describe(error).en)", M("\(Settings.noAsk.icao) could not be selected on the twin's flight board: " + describe(error).en, "\(Settings.noAsk.icao) не удалось выбрать на табло рейсов двойника: " + describe(error).ru)) }
            } else if !fb.a("airports").contains(where: { $0.s("kind") == "custom" && $0.s("code") == Settings.noAsk.icao }) {
                noteOnce("noask-full", M("the twin's custom airports are all used: \(Settings.noAsk.icao) cannot be added - remove one on the twin", "у двойника заняты все свои аэропорты: \(Settings.noAsk.icao) не добавить — удалите один на двойнике"))
            }
        } else if let fb = d["/api/flightboard"],
           let z = fb.a("airports").first(where: { $0.s("kind") == "custom" && $0.s("code") == Settings.noAsk.icao && $0.s("name") == Settings.noAsk.name }),
           let id = z.i("id"), fb.i("airport") != id,
           let panelApts = docs[.panel]?["/api/flightboard"]?.a("airports"), !panelApts.contains(where: { $0.s("code") == Settings.noAsk.icao }) {
            try tw.post("/api/flightboard", ["remove": id]); did.append("\(Settings.noAsk.icao) \"\(Settings.noAsk.name)\" removed")
            routes.append("/api/flightboard")
        }
        guard !did.isEmpty else { return }
        try readDocs(.twin, routes)
        log("override", "twin", M("the owner's override on the twin: " + did.joined(separator: ", "), "переопределение владельца на двойнике: " + did.joined(separator: ", ")))
    }

    /// What a start of the twin by the app did about fbAskHa (main.swift holdAskHa).
    enum AskHaAtStart: Equatable { case set, already, noSetting, notUp, notThisTwin(String), refused(String) }
    /// The owner's override at each start of the twin by the app (item 16, 2026-10-01), whatever the switch of sync says:
    /// fbAskHa off on the twin at D, whose MAC must be MAC, where its firmware has the setting (2.7.13: fbAskHa in
    /// /api/export) and it is on - POST /api/import {"fbAskHa":false} with X-Twin-Sync: 1 (Device.post), as
    /// enforceOverrides writes it while sync is on, then read back. NOTUP: its firmware's web server does not answer yet.
    static func askHaOffAtStart(_ d: Device, mac: String) -> AskHaAtStart {
        guard let info = try? d.get("/api/info") else { return .notUp }
        let m = normMac(info.s("mac"))
        guard m == normMac(mac) else { return .notThisTwin(m) }
        guard let ex = try? d.get("/api/export") else { return .notUp }
        guard isBool(ex["fbAskHa"]) else { return .noSetting }
        if ex.b("fbAskHa") == false { return .already }
        do { try d.post("/api/import", ["fbAskHa": false]) }                     // web.cpp handleImportConfig
        catch { return .refused((error as? SyncError)?.msg.en ?? (error as? SyncDown)?.msg.en ?? "\(error)") }
        guard (try? d.get("/api/export"))?.b("fbAskHa") == false else { return .refused("/api/export still says fbAskHa on") }
        return .set
    }

    /// Whether the twin's flight board is held on ZZZZ "NO REQUESTS" (the owner, 2026-09-30 22:40: a twin without a
    /// key of its own does not ask Home Assistant): its firmware has no fbAskHa to stop the asking (before 2.7.13:
    /// absent from /api/export), a broker is set (mqtt.configured: NVS fb/host, mqtt_bus.cpp:171-186), and it has no
    /// AeroAPI key (direct.key; or no direct fetch built) - so any built-in airport it showed would be asked for, and
    /// HA would fetch it with the owner's key (fb_mqtt.cpp:24-45 mayAsk, 103-136). The key itself is never read:
    /// /api/flightboard says only whether one is stored.
    static func holdsNoAsk(export: J?, board: J?) -> Bool {
        guard let export, export["fbAskHa"] == nil, let fb = board, fb.o("mqtt")?.b("configured") == true else { return false }
        let direct = fb.o("direct") ?? [:]
        return direct.b("built") == false || direct.b("key") != true
    }
    /// What puts the twin's board on ZZZZ (BOARD: its /api/flightboard): nil when it is there already, or when it has
    /// no ZZZZ and no room for one (limits.custom, 6). Selected if the twin has it; else added with the time zone of
    /// the panel's airport (PANEL_BOARD) and selected (web_panel.cpp:517-586, the add's "select"), as the override of
    /// 2026-09-29 did (sync.py until c578579).
    static func noAskHold(board fb: J, panelBoard: J?) -> J? {
        let apts = fb.a("airports")
        if let z = apts.first(where: { $0.s("kind") == "custom" && $0.s("code") == Settings.noAsk.icao }), let id = z.i("id") {
            return fb.i("airport") == id ? nil : ["airport": id]
        }
        let customs = apts.filter { $0.s("kind") == "custom" }.count
        guard customs < (fb.o("limits")?.i("custom") ?? 6) else { return nil }
        let panelApt = panelBoard?.a("airports").first { $0.i("id") == panelBoard?.i("airport") }
        return ["add": ["icao": Settings.noAsk.icao, "iata": "", "name": Settings.noAsk.name,
                        "tz": panelApt?.s("tz") ?? "Europe/Paris", "select": true] as J]
    }

    /// After writing to side O: its screen as it is now becomes its base (a new home city, a removed
    /// effect or a renumbered list can move its page); if its page had been the source's and moved,
    /// the source's page is put back (putBack). A restart it shows stops the round (SyncRestart).
    private func afterWrite(_ o: Side, from s: Side) throws {
        try noRestartPending()
        let before = screen[o]
        let now: Screen
        do { now = try readScreen(o) } catch let r as SyncRestart { throw r } catch { return }
        screen[o] = now; screenAt[o] = Date(); rebaseScreen(o)
        if SyncEngine.putBack(to: o, before: before, now: now, source: screen[s]) { try mirrorScreen(from: s, fields: ["page"]) }
    }
    /// Whether side O's page, moved by a write to it, is put back to the source's: only where O showed the
    /// source's page before and shows another now - not when O's own carousel walks (it moved the page, and
    /// walks on), and never on the leading panel: a show there holds its carousel for the idle time
    /// (panelShowPage -> carouselNote, main.cpp:883-889), and the twin follows the page it is on now anyway -
    /// a renumbered list or a new home city there is the panel's own screen, which the twin takes.
    static func putBack(to o: Side, before: Screen?, now: Screen, source: Screen?) -> Bool {
        guard let before, let src = source, before.shown == src.shown, now.shown != src.shown, !now.running else { return false }
        return !(o == .panel && leader(panel: now) == .panel)
    }

    // MARK: firmware

    /// A new firmware on a side: settled, it is that side's change - wanted on the other side - unless
    /// sync itself sent it there (inFlight), which is then confirmed or found rolled back.
    private func firmwareRound() {
        for s in sides {
            guard let f = fw[s], !f.id.isEmpty else { continue }
            if let fl = inFlight, fl.to == s { checkInFlight(fl, f) ; continue }
            guard let was = firmwareBase[s] else { firmwareBase[s] = f.id; stateDirty = true; continue }
            guard f.id != was else { fwWatchSince[s] = nil; continue }
            guard f.settled else { watch(s, f); continue }
            fwWatchSince[s] = nil
            firmwareBase[s] = f.id; stateDirty = true
            caps[s] = nil; ageHashes(s); noSource[s] = nil; imageCache = nil
            log("note", "firmware", M("\(s.word.en) now runs \(f.id)", "\(s.on) теперь \(f.id)"))
            retryPending()
            // The later change wins: a new firmware on one side replaces what was waiting for the other.
            if s == .panel { wantPanel = nil; wantTwin = fw[.twin]?.id == f.id ? nil : f.id; carry = (0, .distantPast) }
            else { wantTwin = nil; wantPanel = fw[.panel]?.id == f.id ? nil : f.id }
        }
    }

    private func watch(_ s: Side, _ f: Firmware) {
        let since = fwWatchSince[s] ?? Date(); fwWatchSince[s] = since
        if Date().timeIntervalSince(since) < SyncEngine.fwWatchMax { fwWatch = Date().addingTimeInterval(15); return }
        noteOnce("fw-pending-\(s)-\(f.id)", M("\(s.word.en)'s new firmware \(f.id) is still \"\(f.state)\" after five minutes: read again with the settings only",
                                              "новая прошивка \(s.of) \(f.id) всё ещё «\(f.state)» спустя пять минут: дальше проверяю только вместе с настройками"))
    }

    /// The image sync sent to side S: that version - that build, or for a release that size - settled after
    /// a restart since it was sent (uptime), or the old image back with rolledBackFrom, or nothing in five minutes.
    private func checkInFlight(_ fl: InFlight, _ n: Firmware) {
        let since = Date().timeIntervalSince(fl.sent)
        let restarted = Double(n.uptime) <= since + 5
        let s = fl.to, o = fl.from
        if restarted, n.settled, n.version == fl.version, fl.build.map({ $0 == n.build }) ?? (n.bytes == fl.bytes) {
            inFlight = nil; fwWatchSince[s] = nil
            firmwareBase[s] = n.id
            if fw[o]?.id == fl.fromId { firmwareBase[o] = fl.fromId }   // both sides now hold what they run: no echo
            caps[s] = nil; ageHashes(s); noSource[s] = nil; stateDirty = true
            // What is wanted is let go only when it is what was confirmed: a newer build that came to the
            // source meanwhile (firmwareRound, the later change wins) is still wanted, and is carried next.
            if s == .twin { wantTwin = SyncEngine.stillWanted(wantTwin, confirmed: fl.fromId); carry = (0, .distantPast) }
            else { wantPanel = SyncEngine.stillWanted(wantPanel, confirmed: fl.fromId) }
            imageCache = nil
            log(o.arrow, "firmware", M("\(s.word.en) runs \(n.id): confirmed", "\(s.on) \(n.id): подтверждена"))
            retryPending()
            if s == .twin { twinFollowsPanel() }
            saveState()
            return
        }
        if restarted, n.settled, n.id == fl.oldId, !n.rolledBackFrom.isEmpty {
            inFlight = nil; firmwareBase[s] = n.id; stateDirty = true
            failedCarry(s, SyncError(M("\(s.word.en) rolled back to \(n.id): the new image did not confirm itself", "\(s.on) откат на \(n.id): новый образ не подтвердил себя")))
            return
        }
        if since > SyncEngine.confirmWithin {
            inFlight = nil; stateDirty = true
            failedCarry(s, SyncError(M("\(s.word.en) did not confirm \(fl.version) within five minutes", "\(s.on) за пять минут не подтвердилась \(fl.version)")))
            return
        }
        fwWatch = Date().addingTimeInterval(10)
    }

    /// The build still wanted once CONFIRMED was confirmed on the target: none if it was that one.
    static func stillWanted(_ want: String?, confirmed: String) -> String? { want == confirmed ? nil : want }

    /// Whether a failed transfer to the twin is tried again, and after how long: nil after CARRY_TRIES
    /// failures in a row - then only "Sync now" tries again.
    static func carryWait(afterFailures n: Int) -> TimeInterval? {
        n >= CARRY_TRIES ? nil : carryBackoff[min(max(n, 1) - 1, carryBackoff.count - 1)]
    }

    /// What is wanted and due: the panel's firmware to the twin (tried again after a pause when it fails,
    /// CARRY_TRIES times at most), the twin's to the panel as a question (again on "Sync now" and at the next start).
    private func firmwareDue() throws {
        guard inFlight == nil else { return }
        if let w = wantTwin {
            if fw[.panel]?.id != w || fw[.twin]?.id == w { wantTwin = nil; stateDirty = true }
            else if carry.tries < SyncEngine.CARRY_TRIES, Date() >= carry.next {
                do { try updateFirmware(from: .panel, offer: nil) }
                catch let e where e is SyncStopped || e is SyncWait || e is SyncRestart { throw e }
                catch { failedCarry(.twin, error) }
            }
        }
        if let w = wantPanel {
            if fw[.twin]?.id != w || fw[.panel]?.id == w { wantPanel = nil; stateDirty = true }
            // Not while the panel's own firmware is still proving itself (just flashed, pending or new): the
            // later change wins, and it may be the panel's.
            else if offer == nil, !declined.contains(w), fw[.panel]?.settled != false, fwWatchSince[.panel] == nil { offerToPanel() }
        }
    }

    private func failedCarry(_ to: Side, _ e: Error) {
        if to == .twin {
            carry.tries += 1
            guard let wait = SyncEngine.carryWait(afterFailures: carry.tries) else {
                carry.next = .distantFuture
                log("error", "firmware", M("the panel's firmware is not on the twin: \(describe(e).en) - \(carry.tries) attempts failed: not tried again until Sync now",
                                           "прошивка панели не перенесена на двойника: \(describe(e).ru) — неудачных попыток \(carry.tries): больше не пробую до «Синхронизировать сейчас»"),
                    problem: true, sticky: true)
                return
            }
            carry.next = Date().addingTimeInterval(wait)
            log("error", "firmware", M("the panel's firmware is not on the twin: \(describe(e).en) - tried again in \(Int(wait / 60)) min",
                                       "прошивка панели не перенесена на двойника: \(describe(e).ru) — повторю через \(Int(wait / 60)) мин"), problem: true)
        } else {
            if let w = wantPanel { declined.insert(w) }
            log("error", "firmware", M("the panel was not updated: \(describe(e).en) - asked again on Sync now or at the next start",
                                       "панель не обновлена: \(describe(e).ru) — спрошу снова по «Синхронизировать сейчас» или при следующем запуске"), problem: true)
        }
    }

    /// "Update the panel too?", with what the answer is bound to.
    private func offerToPanel() {
        let t = fw[.twin]!, p = fw[.panel]!
        let source = imageCap(.twin)?.has == true ? M("read from the twin (/api/firmware/image)", "берётся с двойника (/api/firmware/image)")
                                                   : M("the release v\(t.version) of \(repo) on GitHub", "выпуск v\(t.version) из \(repo) на GitHub")
        let downgrade = versionNumbers(t.version).flatMap { a in versionNumbers(p.version).map { a.lexicographicallyPrecedes($0) } } ?? false
        let o = Offer(id: UUID().uuidString, pair: pairKey, panelName: p.name, panelAddress: device(.panel).address, panelMac: p.mac,
                      panelVersion: p.version, panelBuild: p.build, panelId: p.id, twinId: t.id, version: t.version, build: t.build,
                      source: source, downgrade: downgrade, pressForTesting: testInt("syncPressReturnForTesting"))
        offer = o
        if o.pressForTesting == 0 && testSwitch("syncAutoConfirmForTesting") {
            let delay = max(0, UserDefaults.standard.double(forKey: "syncAnswerDelayForTesting"))
            log("note", "firmware", M("the panel is a twin and syncAutoConfirmForTesting is on: both windows are answered yes by itself in \(Int(delay)) s",
                                      "«панель» — двойник, и включён syncAutoConfirmForTesting: «да» в обоих окнах дам сам через \(Int(delay)) с"))
            DispatchQueue.global().asyncAfter(deadline: .now() + delay) { [weak self] in self?.locked { self?._fwReply = (o.id, true) } }
            return
        }
        log("note", "firmware", M("asking whether to update the panel \(p.name) (\(o.panelAddress), \(p.mac)) from \(p.id) to \(t.id)\(downgrade ? " - a downgrade" : "")",
                                  "спрашиваю, обновлять ли панель \(p.name) (\(o.panelAddress), \(p.mac)) с \(p.id) до \(t.id)\(downgrade ? " — это понижение версии" : "")"))
        DispatchQueue.main.async { self.askFirmware?(o) }
    }

    /// The person's answer to "Update the panel too?" and "Really flash the physical panel?" (yes only when
    /// both were yes). A yes counts only for what it was given for: the same panel (MAC), the same firmware
    /// on it, the same on the twin - read again now, and once more right before the image is sent
    /// (updateFirmware). Otherwise nothing is sent and the question is asked again, with what holds then.
    private func answeredFirmware(id: String, go: Bool) throws {
        guard let o = offer, o.id == id else { return }
        offer = nil
        guard go else {
            declined.insert(o.twinId)
            log("note", "firmware", M("the panel stays on its firmware (not now): asked again on Sync now or at the next start",
                                      "панель остаётся на своей прошивке (не сейчас): спрошу снова по «Синхронизировать сейчас» или при следующем запуске"))
            return
        }
        try verifyIdentity()                                                  // the MACs, and fw[] read again
        guard offerHolds(o) else { return }
        do { try updateFirmware(from: .twin, offer: o) }
        catch let e where e is SyncStopped || e is SyncWait || e is SyncRestart { throw e }
        catch { failedCarry(.panel, error) }
    }

    /// Whether the pair and both firmwares are still what the person said yes to (fw[] just read). If not,
    /// nothing is sent: the change is looked at first, and the question comes again if they still differ.
    private func offerHolds(_ o: Offer) -> Bool {
        let p = fw[.panel]!, t = fw[.twin]!
        if pairKey == o.pair, p.mac == o.panelMac, p.id == o.panelId, t.id == o.twinId { return true }
        log("note", "firmware", M("changed since the yes (the panel runs \(p.id), the twin \(t.id)): the panel is not flashed - asked again if they still differ",
                                  "с момента «да» изменилось (на панели \(p.id), на двойнике \(t.id)): панель не прошиваю — если прошивки всё ещё различаются, спрошу заново"), problem: true)
        nextSlow = .distantPast                                              // the change is looked at first, then asked about
        return false
    }

    /// The image of side S's firmware, sent to the other side; its confirmation is watched by firmwareRound.
    /// To the panel only with OFFER, the person's yes, which must still hold right before the image goes.
    private func updateFirmware(from s: Side, offer yes: Offer?) throws {
        let f = fw[s]!, o = s.other, targetWas = fw[o]!.id
        precondition(o == .twin || yes != nil, "the panel is flashed only on a person's yes")
        // Whether it fits the target's OTA slot, from the size /api/info gives - before anything is read.
        let free = fw[o]?.otaFree ?? 0
        guard free == 0 || f.bytes <= free else {
            throw SyncError(M("the image (\(f.bytes) B) does not fit \(o.word.en)'s OTA slot (\(free) B)", "образ (\(f.bytes) Б) не помещается в OTA-раздел \(o.of) (\(free) Б)"))
        }
        phase(M("updating the firmware of \(o.word.en) to \(f.version)", "обновляю прошивку \(o.of) до \(f.version)"))
        let (image, fromRelease) = try firmwareImage(from: s, f)
        guard free == 0 || image.count <= free else {
            throw SyncError(M("the image (\(image.count) B) does not fit \(o.word.en)'s OTA slot (\(free) B)", "образ (\(image.count) Б) не помещается в OTA-раздел \(o.of) (\(free) Б)"))
        }
        let hold = testInt("syncImageHoldForTesting")
        if hold > 0 {
            log("note", "firmware", M("image read: holding \(hold) s before the last look (syncImageHoldForTesting)", "образ прочитан: жду \(hold) с до последней проверки (syncImageHoldForTesting)"))
            let until = Date().addingTimeInterval(TimeInterval(hold))
            while Date() < until { guard writesAllowed else { throw SyncStopped() }; Thread.sleep(forTimeInterval: 0.5) }
        }
        // The last look before the firmware goes: the switch, the pair, and the firmware on both sides - for
        // the panel what the person said yes to, for the twin what the image was read for. The image took
        // time to read (up to 180 s from the twin or GitHub), and the panel may have been flashed meanwhile.
        guard writesAllowed else { throw SyncStopped() }
        try verifyIdentity()
        if let yes {
            guard offerHolds(yes) else { return }
        } else if fw[s]!.id != f.id || fw[o]!.id != targetWas {
            log("note", "firmware", M("changed while the image was read (\(s.word.en) runs \(fw[s]!.id), \(o.word.en) \(fw[o]!.id)): not sent - the change is looked at first",
                                      "пока читался образ, изменилось (\(s.on) \(fw[s]!.id), \(o.on) \(fw[o]!.id)): не отправляю — сначала разберу изменение"))
            nextSlow = .distantPast
            return
        }
        inFlight = InFlight(to: o, from: s, version: f.version, bytes: image.count, build: fromRelease ? nil : f.build, fromId: f.id,
                            oldId: fw[o]?.id ?? "", sent: Date())
        saveState()
        let a: Answer
        do { a = try device(o).upload("/update", query: [], field: "firmware", filename: "firmware.bin", data: image, timeout: 300) }
        catch { inFlight = nil; stateDirty = true; throw error }
        guard a.status == 200 else { inFlight = nil; stateDirty = true; throw device(o).refused("POST /update", a) }
        inFlight!.sent = Date(); stateDirty = true
        fwWatch = Date().addingTimeInterval(10)
        log(s.arrow, "firmware", M("\(f.id)\(fromRelease ? " (the release)" : "") sent to \(o.word.en); it restarts and confirms the image within about a minute",
                                   "\(f.id)\(fromRelease ? " (выпуск)" : "") отправлена \(o == .panel ? "на панель: она перезагружается и подтверждает" : "двойнику: он перезагружается и подтверждает") образ примерно за минуту"))
    }

    /// The image, and whether it is the release (confirmed then by version and size, not build). One read
    /// is kept, by the source's firmware and its ELF SHA-256, until it is confirmed on the target or the
    /// source runs another: a transfer tried again does not read the panel again.
    private func firmwareImage(from s: Side, _ f: Firmware) throws -> (Data, Bool) {
        guard let cap = imageCap(s) else {
            throw SyncError(M("whether \(s.word.en) gives out its image is not known now", "отдаёт ли \(s.word.ru) свой образ, сейчас не узнать"))
        }
        let key = "\(s)|\(f.id)|\(cap.elf ?? "-")"
        if let c = imageCache, c.key == key {
            log("note", "firmware", M("the image of \(f.id) read before is used again: not read again", "образ \(f.id), прочитанный раньше, беру снова: повторно не читаю"))
            return (c.data, c.fromRelease)
        }
        let (d, rel) = try readImage(from: s, f, cap)
        imageCache = (key, d, rel)
        return (d, rel)
    }

    private func readImage(from s: Side, _ f: Firmware, _ cap: (has: Bool, elf: String?)) throws -> (Data, Bool) {
        // Reading the panel holds its loop() for the whole transfer: the release is taken when it is that build.
        if s == .panel || !cap.has {
            var rel: GitHub.Release?
            do { rel = try GitHub.release(tag: "v" + f.version) } catch { if !cap.has { throw error } }
            if let rel, rel.size == f.bytes {
                var d: Data?
                do { d = try GitHub.image(rel) } catch { if !cap.has { throw error } }
                if let d {
                    if !cap.has { return (d, true) }
                    if let elf = cap.elf, appElfSha256(d) == elf {
                        log("note", "firmware", M("\(s.word.en) runs the release v\(f.version) itself (the same ELF SHA-256): taken from GitHub",
                                                  "\(s.on) сам выпуск v\(f.version) (тот же ELF SHA-256): беру его с GitHub"))
                        return (d, true)
                    }
                }
            }
            if !cap.has {
                throw SyncError(M("\(s.word.en)'s firmware has no /api/firmware/image, and \(repo) has no release v\(f.version) of \(f.bytes) B",
                                  "в прошивке \(s.of) нет /api/firmware/image, а в \(repo) нет выпуска v\(f.version) размером \(f.bytes) Б"))
            }
        }
        let a = try device(s).firmwareImage()
        guard a.header("X-Firmware-Version") == f.version, a.data.count == f.bytes else {
            throw SyncError(M("the image read is \(a.data.count) B of \(a.header("X-Firmware-Version") ?? "?"), the device says \(f.bytes) B of \(f.version)",
                              "прочитан образ \(a.data.count) Б версии \(a.header("X-Firmware-Version") ?? "?"), а устройство говорит \(f.bytes) Б версии \(f.version)"))
        }
        try checkImage(a.data)
        guard let elf = a.header("X-App-Elf-Sha256")?.lowercased(), appElfSha256(a.data) == elf else {
            throw SyncError(M("the image's ELF SHA-256 is not the X-App-Elf-Sha256 it came with", "ELF SHA-256 в образе не совпадает с X-App-Elf-Sha256 ответа"))
        }
        // The build HEAD named when the source's firmware was looked at: another one now is another image.
        if let e = cap.elf, e != elf {
            caps[s] = nil
            throw SyncError(M("the image read is another build than HEAD named (ELF SHA-256 \(elf.prefix(12))…, not \(e.prefix(12))…): not taken",
                              "прочитанный образ — другая сборка, чем назвал HEAD (ELF SHA-256 \(elf.prefix(12))…, а не \(e.prefix(12))…): не беру"))
        }
        return (a.data, false)
    }

    // MARK: kept between runs

    private struct Saved: Codable {
        var v: Int?                                   // 2 (app 1.3): settings as HMAC digests under StateKey; 3 (1.4): and the consent
        var key: String?                              // StateKey.tag of the key they were made with
        var pair: String
        var settings: [String: [String: String]]
        var effects: [String: [String: String]]
        var firmware: [String: String]
        var pending: [String: [String: Int]]?        // effects not carried yet, by the side that has them
        var wantPanel: String?, wantTwin: String?     // a firmware one side should get
        var inFlight: InFlight?
        var consent: String?                          // consentToken(pair): both windows answered, the switch not off since
        var refused: [String: [String: Refusal]]?     // effects a side refused, by that side (refusedFx)
        var hashes: [String: [String: SavedHash]]?    // 1.4.1: each side's scripts' SHA-256, by file name (hashes)
    }
    /// A script's SHA-256 as kept, with the size it was read for. When it was read is not kept: after a start each is read
    /// again on the schedule, one at a time, the first ones first (hashNext) - so a file that changes only when a hash
    /// does is not written every few seconds.
    private struct SavedHash: Codable { var bytes: Int; var hash: String }

    /// The consent kept for PAIR: an HMAC under StateKey, like the settings' digests - so it cannot be written into
    /// the file for another pair, or by anything without the Keychain's key.
    static func consentToken(_ pair: String) -> String {
        hex(HMAC<SHA256>.authenticationCode(for: Data("consent|\(pair)".utf8), using: StateKey.key).prefix(16))
    }
    /// Whether a saved state of this very pair (its MACs; loadState checks) gives the first window's answer (the owner's
    /// item 15, 2026-10-01): only when the switch was on as the app started and has stayed on (LAUNCH_ON), and the pair
    /// was aligned (ALIGNED: the saved state has its settings). Then either the state is app 1.3's (VERSION 2, which
    /// kept no consent: its first start of 1.4 over it), or it carries this pair's consent (TOKEN, written by 1.4 while
    /// the switch was on and removed when it was switched off).
    static func keptConsent(version: Int?, token: String?, pair: String, launchOn: Bool, aligned: Bool) -> Bool {
        guard launchOn, aligned else { return false }
        if version == 2 { return true }
        return version == 3 && token != nil && token == consentToken(pair)
    }

    private func saveState() {
        guard StateKey.kept, aligned, !pairKey.isEmpty else { return }
        var s = Saved(v: 3, key: StateKey.tag, pair: pairKey, settings: [:], effects: [:], firmware: [:], pending: [:],
                      wantPanel: wantPanel, wantTwin: wantTwin, inFlight: inFlight,
                      consent: consented ? SyncEngine.consentToken(pairKey) : nil, refused: [:],
                      hashes: Dictionary(uniqueKeysWithValues: sides.map { ($0.rawValue, (hashes[$0] ?? [:]).mapValues { SavedHash(bytes: $0.bytes, hash: $0.hash) }) }))
        for side in sides {
            s.settings[side.rawValue] = base[side]?["settings"] ?? [:]
            s.effects[side.rawValue] = base[side]?["effects"] ?? [:]
            s.firmware[side.rawValue] = firmwareBase[side] ?? ""
            s.pending?[side.rawValue] = pendingFx[side] ?? [:]
            s.refused?[side.rawValue] = refusedFx[side] ?? [:]
        }
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]; enc.dateEncodingStrategy = .iso8601
        if let d = try? enc.encode(s), d != lastSaved { try? d.write(to: stateFile, options: .atomic); lastSaved = d }
        stateDirty = false
    }

    /// Sync switched off: the consent in sync-state.json is forgotten, whatever pair it is for - the next switch-on
    /// asks both questions. The rest of the state stays (a resume shows what changed meanwhile).
    private func dropSavedConsent() {
        guard let d = try? Data(contentsOf: stateFile), let (out, v2) = SyncEngine.consentDropped(d) else { return }
        try? out.write(to: stateFile, options: .atomic); lastSaved = out
        log("note", "consent", v2 ? M("sync-state.json of app 1.3 is made 1.4's with no consent in it: switching sync on asks both questions",
                                      "sync-state.json версии 1.3 переписан в формат 1.4 без согласия: при включении синхронизации задам оба вопроса")
                                  : M("the consent kept in sync-state.json is forgotten: switching sync on asks both questions",
                                      "согласие, сохранённое в sync-state.json, забыто: при включении синхронизации задам оба вопроса"))
    }
    /// The state file D with no consent in it - 1.4's token removed, and app 1.3's file (v2, whose aligned state is taken
    /// for the consent at 1.4's first start with the switch on, keptConsent) made 1.4's (v3), the bases kept - and whether
    /// it was 1.3's. Nil when D gives no consent already (or is not a state file). Without this a v2 file outlived the
    /// switch turned off, and a later start with the switch on took it for the consent again: sync went on with no window
    /// (the review of 2026-10-01, C1).
    static func consentDropped(_ d: Data) -> (Data, Bool)? {
        let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
        guard var s = try? dec.decode(Saved.self, from: d), s.consent != nil || s.v == 2 else { return nil }
        let v2 = s.v == 2
        s.consent = nil; s.v = 3
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]; enc.dateEncodingStrategy = .iso8601
        return (try? enc.encode(s)).map { ($0, v2) }
    }

    private func loadState() {
        guard StateKey.kept else {
            noteOnce("no-key", M("the Keychain does not give the sync state's key: nothing is kept between runs, and each start asks the direction",
                                 "Связка ключей не отдаёт ключ состояния синхронизации: между запусками ничего не сохраняется, и при каждом запуске спрошу направление"))
            return
        }
        let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
        guard let d = try? Data(contentsOf: stateFile), let s = try? dec.decode(Saved.self, from: d), s.pair == pairKey else { return }
        guard s.v == 2 || s.v == 3, s.key == StateKey.tag else {
            log("note", "state", M("sync-state.json was written by an earlier version or with another key: not used - the direction is asked",
                                   "sync-state.json записан прежней версией или другим ключом: не использую — спрошу направление"))
            return
        }
        for side in sides {
            base[side, default: [:]]["settings"] = s.settings[side.rawValue] ?? [:]
            base[side, default: [:]]["effects"] = s.effects[side.rawValue] ?? [:]
            if let f = s.firmware[side.rawValue], !f.isEmpty { firmwareBase[side] = f }
            if let p = s.pending?[side.rawValue], !p.isEmpty { pendingFx[side] = p }
            if let r = s.refused?[side.rawValue], !r.isEmpty { refusedFx[side] = r }
        }
        wantPanel = s.wantPanel; wantTwin = s.wantTwin; inFlight = s.inFlight
        // The scripts as read before (1.4.1): no batch of reads holds the panel's loop() or loads the twin's engine at a
        // start - a script whose size is the same is taken as read, and read again on the schedule; one whose size changed
        // meanwhile goes first. A question reads them all (readAll settle).
        for side in sides { if let h = s.hashes?[side.rawValue], !h.isEmpty { hashes[side] = h.mapValues { ($0.bytes, $0.hash, Date.distantPast) } } }
        resumed = !(s.settings["panel"] ?? [:]).isEmpty
        let kept = SyncEngine.keptConsent(version: s.v, token: s.consent, pair: pairKey, launchOn: locked({ _launchOn }), aligned: resumed)
        if s.v == 2 {
            // App 1.3's file stands for the consent once at most, at this first start of 1.4: it is 1.4's from now on, with
            // this pair's consent in it when it counted, else with none.
            var u = s; u.v = 3; u.consent = kept ? SyncEngine.consentToken(pairKey) : nil
            let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]; enc.dateEncodingStrategy = .iso8601
            if let out = try? enc.encode(u) { try? out.write(to: stateFile, options: .atomic); lastSaved = out }
        }
        if kept {
            consented = true; consentKept = true
            log("note", "consent", s.v == 2
                ? M("the switch was on as the app started, and sync-state.json holds this pair's aligned state from app 1.3 (\(pairKey)): its consent counts as given - no \"Enable sync?\" window; the direction is asked only if the twin changed meanwhile",
                    "переключатель был включён при запуске, а в sync-state.json — выровненное состояние этой пары от версии 1.3 (\(pairKey)): согласие считается данным — окна «Включить синхронизацию?» нет; направление спрошу, только если двойник за это время менялся")
                : M("the switch stayed on, and the consent for this pair (\(pairKey)) is kept from before the restart: no \"Enable sync?\" window; the direction is asked only if the twin changed meanwhile",
                    "переключатель не выключали, согласие для этой пары (\(pairKey)) сохранено с прошлого запуска: окна «Включить синхронизацию?» нет; направление спрошу, только если двойник за это время менялся"))
        }
    }
}

/// The ESP application image's own checks: magic 0xE9, chip id 9 (ESP32-S3) at bytes 12-13, byte 23
/// (hash_appended) 1, and the SHA-256 of everything before the last 32 bytes equal to them
/// (esp_image_header_t; IDF 4.4 esp_image_format.c). Every image the firmware's build makes has it.
func checkImage(_ d: Data) throws {
    let b = [UInt8](d)
    guard b.count > 64, b[0] == 0xE9 else { throw SyncError(M("not an ESP application image", "это не образ приложения ESP")) }
    guard Int(b[12]) | Int(b[13]) << 8 == 9 else { throw SyncError(M("the image is not for the ESP32-S3", "образ не для ESP32-S3")) }
    guard b[23] == 1 else { throw SyncError(M("the image carries no SHA-256 of its own: not taken", "у образа нет собственного SHA-256: не беру")) }
    let body = Data(b[0 ..< b.count - 32]), tail = Data(b[(b.count - 32)...])
    guard Data(SHA256.hash(data: body)) == tail else { throw SyncError(M("the image's own SHA-256 does not match: damaged", "собственный SHA-256 образа не сходится: образ повреждён")) }
}

/// The instant events of firmware 2.7.13 (src/sync/sync_events.cpp): a side asked by POST /api/sync/listen sends a small
/// JSON datagram from its UDP port 4210 on every change of its screen and every gesture. One socket for both sides, the
/// same port every run (47261, else any) - an app started again renews its place on a device rather than taking the
/// second one. Each datagram goes to the engine's worker as it is (SyncEngine.received); nothing here judges it.
final class EventListener {
    private(set) var port: UInt16 = 0
    private let fd: Int32
    private let onDatagram: (Data, String, UInt16) -> Void

    init?(port wanted: UInt16 = 47261, onDatagram: @escaping (Data, String, UInt16) -> Void) {
        let sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard sock >= 0 else { return nil }
        func bindTo(_ p: UInt16) -> Bool {
            var a = sockaddr_in()
            a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
            a.sin_port = p.bigEndian; a.sin_addr.s_addr = INADDR_ANY
            return withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(sock, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } == 0
        }
        if !bindTo(wanted) && !bindTo(0) { close(sock); return nil }
        var a = sockaddr_in(); var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(sock, $0, &len) } }
        fd = sock; self.onDatagram = onDatagram; port = UInt16(bigEndian: a.sin_port)
        let t = Thread { [weak self] in self?.loop() }
        t.name = "twin-sync-events"; t.qualityOfService = .userInitiated; t.start()
    }

    /// An empty datagram to 127.0.0.1:PORT, a device's UDP port forwarded by the engine's NAT (--hostfwd udp:PORT-4210):
    /// the engine sends what the device sends to the gateway's PORT from 4210 to whoever sent to PORT last (esp-soc/src/
    /// nat.rs udp_out). The firmware takes nothing from an empty datagram (network.cpp handleUDP: parsePacket() is 0).
    func prime(_ to: UInt16) {
        var a = sockaddr_in()
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
        a.sin_port = to.bigEndian; a.sin_addr.s_addr = inet_addr("127.0.0.1")
        var none: UInt8 = 0
        _ = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
            sendto(fd, &none, 0, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
    }

    private func loop() {
        var buf = [UInt8](repeating: 0, count: 1500)
        var from = sockaddr_in()
        while true {
            var len = socklen_t(MemoryLayout<sockaddr_in>.size)
            let n = withUnsafeMutablePointer(to: &from) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                recvfrom(fd, &buf, buf.count, 0, $0, &len) } }
            if n < 0 { if errno == EINTR { continue }; return }
            guard n > 0 else { continue }
            var ip = from.sin_addr; var text = [CChar](repeating: 0, count: Int(INET_ADDRSTRLEN))
            guard inet_ntop(AF_INET, &ip, &text, socklen_t(INET_ADDRSTRLEN)) != nil else { continue }
            onDatagram(Data(buf[0..<n]), String(cString: text), UInt16(bigEndian: from.sin_port))
        }
    }
}
