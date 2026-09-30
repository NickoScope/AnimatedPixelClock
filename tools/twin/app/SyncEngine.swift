// SyncEngine.swift - Sync with panel: the app's twin and one physical panel kept as one device.
//
// The owner, 2026-09-30 09:15: while the switch is on, the screen (page, effect, clock style,
// brightness, on/off), the settings, the Lua effects (a new one appears, a removed one goes) and the
// firmware are mirrored both ways. Firmware goes to the twin by itself and to the physical panel only
// after a person says yes here ("Update the panel too?"). What sync brings is not sent back (no echo);
// when both sides change the same thing, the later change wins. The owner's one override on the twin,
// climateHa off (2026-09-29, tools/twin/sync.py OVERRIDES), holds always and never travels to the panel;
// secrets - the weather API key, and the AeroAPI, RTT and AIS keys of the portal's Keys page - identity
// and state never travel at all. Other owners work in their own forks: firmwareRepo names where releases
// and the gallery come from.
// The owner, 2026-09-30 19:35: the twin keeps no paid screen off by force. Like the panel, it has the Keys
// page (data-page="pkeys", /api/keys: a key is written there and never given out - its GET says only
// whether one is stored, web_panel.cpp:1000-1123), and its flight and rail boards work from the keys
// entered there. So the flights and trains pages, their place in the walk and the flight board's airport
// are mirrored like everything else; the keys never are, in either direction - each device has its own.
// Without a key the board makes no paid call of its own: the AeroAPI fetch stops before anything is counted
// or sent (aero_direct.cpp:725, NO KEY), the RTT one too (rtt_direct.cpp:737, NO TOKEN). A twin with an MQTT
// broker in its NVS (never set by sync) and no AeroAPI key asks Home Assistant for a built-in airport's
// board, as a panel without a key does (fb_mqtt.cpp:103-136; once per airport and half in 15 min, 12 an
// hour at most, :24-45) - and HA pays for that fetch with its own key.
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
//
// The firmware has no push channel - no WebSocket, no event stream, no MQTT topic with the screen's
// state - so both devices are polled over HTTP, one request at a time: the firmware's WebServer serves
// a request inside loop() and the panel stands still meanwhile (web_panel.cpp:1066-1068), and a burst
// of parallel requests once drained the radio's memory (web_heap_backoff.h:4-12). Every request here
// comes from one worker thread, and requests to the panel are paced. A 503 is asked again only when it
// carries Retry-After: that is the firmware standing aside before doing anything (webRefuseBig,
// web.cpp:1421-1456). The Panel group's 503 "out of memory" comes after the work was done, with no
// Retry-After (failOom, web_panel.cpp:138-152; sendDocFromPsram, web.cpp:99-104): a POST that got it is
// not sent again - the round fails, and the next one reads the target again and decides from that.
//
// What is read, and how often (the periods are our choice, not measured; the portal itself polls
// /api/panel every 2 s and /api/info every 5 s while it is open, web_panel_js.h:86, web_pages.h:2376):
//   screen    every 3 s   GET /api/panel (web_panel.cpp:309-393: now.key/name/style/entered, the carousel -
//                         enabled, idleS, slotS, allStyles, running, holdS, pageS, nextS - pages[]) and GET
//                         /api/status (web.cpp:554-580: brightness %, forcedOff, uptime - a smaller uptime is
//                         a reboot, whose reset screen is nobody's change)
//   step      the panel's GET /api/panel alone, right after its carousel's next step is due (nextS), while
//                         the twin follows it; it stands for the next screen round ("One carousel for both")
//   who       every 15 s  GET /api/info of both: the MAC must still be the pair's; also at once after a
//                         device answered again and when its uptime jumped (an address changes hands)
//   effects   when the Lua names or walk switches in /api/panel change, and every 60 s: GET /api/lua
//                         (web_panel.cpp:968-1062; slow, about 0.5 s on the twin); where the firmware
//                         gives out the scripts (GET /api/lua/source), each one's SHA-256, read again
//                         when its size changes, or on the panel every 5 min, on the twin every round
//                         (our choice: each read holds the panel's loop())
//   settings  every syncSettingsEveryS (60 s): /api/info, /api/portal, /api/export, /api/knob,
//             /api/worldclock, /api/railboard, /api/flightboard, /api/media, /api/ir; /api/market (30 KB)
//             every fifth time. /api/info also carries the firmware: version, build, firmwareBytes,
//             ota.state (web.cpp:431-551), read every 15 s while a new image is still "pending" or
//             "new" (boot_health.cpp:36-45), five minutes at most.
//   load      /api/info's webRefused and allocFails (web.cpp:477-485): when either grew since the last
//             round the panel is under strain, and for the next 10 min (our choice) the screen is read
//             every 5 s (the portal's own cadence, net_turns.h:32) and the settings round leaves 1.6 s
//             between the panel's requests - more than NET_TURN_QUIET_MS, so the panel's own fetches are
//             not held back by it (net_turns.h:47).
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
// screen, and nothing is written to the panel - nor its settings: what differs on it after the restart is
// taken for writes it lost (a module saves to NVS 2.5 s after a change, panel.cpp panelTick), and the panel's
// values go back to it (settingsChanges). Sync switched off: nothing is rolled back, and each carousel walks
// by itself once the twin's hold ends.
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
// second device in Home Assistant; it is left out of what is compared and put back on the twin whenever
// it drifts. The overrides of 2026-09-29 that kept the trains and flights pages out of the twin's walk and
// its flight board on ZZZZ "NO REQUESTS" are gone (the owner, 2026-09-30 19:35): while the twin still
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

enum Side: String, Codable {
    case panel, twin
    var other: Side { self == .panel ? .twin : .panel }
    var word: Msg { self == .panel ? M("the panel", "панель") : M("the twin", "двойник") }
    /// "на панели" / "на двойнике", "у панели" / "у двойника", "панели" / "двойника".
    var on: String { self == .panel ? "на панели" : "на двойнике" }
    var at: String { self == .panel ? "у панели" : "у двойника" }
    var of: String { self == .panel ? "панели" : "двойника" }
    var to: String { self == .panel ? "панель" : "двойника" }
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
                                     "/api/flightboard", "/api/market", "/api/media"]
    // The GETs that change something, each with the only parameters it may carry.
    static let actions: [String: Set<String>] = ["/api/display/on": [], "/api/display/off": [], "/api/display/brightness": ["value"],
                                                 "/api/ir/fn": ["btn", "fn", "page"], "/api/log": ["on"]]
    /// The sync routes (feat/sync-routes). GET|HEAD /api/firmware/image answers only a request with
    /// X-Twin-Sync: 1, else 403 before the image is touched (web.cpp handleFirmwareImage) - a header no web
    /// page can send to another address without a preflight, which the firmware does not answer. GET
    /// /api/lua/source needs no header and answers any page, with Access-Control-Allow-Origin: *
    /// (web_panel.cpp handleLuaSource: the twin's panel page reads a script's header from it). The header
    /// is sent on both. Each read of them holds the panel's loop() while it runs, so switching sync off
    /// cuts them off as it cuts off a write, and none is started while it is off.
    static let syncRoutes: Set<String> = ["/api/lua/source", "/api/firmware/image"]

    func writes(_ method: String, _ path: String) -> Bool { !(method == "GET" || method == "HEAD") || Device.actions[path] != nil }
    /// Stopped by the switch: every write, and the reads of the sync routes.
    func stoppable(_ method: String, _ path: String) -> Bool { writes(method, path) || Device.syncRoutes.contains(path) }

    /// One request. 503 with Retry-After is asked again (the firmware stood aside before doing anything);
    /// 503 without it is the answer. A transport fault is retried for a request that only reads, up to
    /// ATTEMPTS in all - a POST may have landed.
    func send(_ method: String, _ path: String, query: [(String, String)] = [], body: Data? = nil, type: String? = nil,
              timeout: TimeInterval = 15, limit: Int = 2 << 20, attempts: Int = 3) throws -> Answer {
        var c = URLComponents(string: "http://\(address)\(path)")!
        if !query.isEmpty { c.queryItems = query.map { URLQueryItem(name: $0.0, value: $0.1) } }
        guard let url = c.url else { throw SyncError(M("bad address \(address)", "неверный адрес \(address)")) }
        let write = writes(method, path), stop = stoppable(method, path), allowed = mayWrite
        var attempt = 0
        while true {
            attempt += 1
            if stop && !allowed() { throw SyncStopped() }
            if write { willWrite() }
            var req = URLRequest(url: url, timeoutInterval: timeout)
            req.httpMethod = method; req.httpBody = body
            if let type { req.setValue(type, forHTTPHeaderField: "Content-Type") }
            if Device.syncRoutes.contains(path) { req.setValue("1", forHTTPHeaderField: "X-Twin-Sync") }
            if pace > 0 { let wait = lastEnd.addingTimeInterval(pace).timeIntervalSinceNow; if wait > 0 { Thread.sleep(forTimeInterval: wait) } }
            let (a, err, stopped) = Device.exchange(req, limit: limit, cancel: stop ? { !allowed() } : nil)
            lastEnd = Date()
            if stopped { throw SyncStopped() }
            if let a, a.status == 503, let wait = a.retryAfter, attempt < 6 {
                Thread.sleep(forTimeInterval: min(wait, 30) + 0.4 * Double(attempt)); continue
            }
            if let a { return a }
            if !write, attempt < attempts { Thread.sleep(forTimeInterval: 0.8); continue }
            throw SyncDown(side: side, msg: M("\(side.word.en) at \(address) does not answer (\(err ?? "?"))",
                                              "\(side.word.ru) по адресу \(address) не отвечает (\(err ?? "?"))"))
        }
    }

    /// The request, waited for in steps of 0.2 s: CANCEL true cuts it off (the third value says so).
    static func exchange(_ req: URLRequest, limit: Int, cancel: (() -> Bool)? = nil) -> (Answer?, String?, Bool) {
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
        while sem.wait(timeout: .now() + 0.2) == .timedOut {
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

    func luaSource(_ stem: String) throws -> Data {
        let a = try send("GET", "/api/lua/source", query: [("name", stem)], timeout: 30, limit: 1 << 20)
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

// MARK: - what a device holds

struct Screen {
    var key = "", name = "", page = 0, style = 0, bright = 0, off = false, uptime = 0
    var running = false, allStyles = false, pageS = 0
    /// The carousel switched on (its setting: walking, or held after a person's choice), its idle time, the
    /// seconds it is still held (rounded up), the seconds to its next step, when it is on and the page has a
    /// time (web_panel.cpp:345-365: max(secs - pageS, holdS), pageS rounded down); a page entered with the
    /// knob (now.entered).
    var enabled = false, idleS = 0, holdS = 0, nextS: Int? = nil, entered = false
    var pages: [J] = [], styles: Set<Int> = []
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
}

struct Effects {
    var bytes: [String: Int] = [:]      // shown name -> size of the uploaded script
    var hash: [String: String] = [:]    // shown name -> SHA-256 of its bytes, where the device gives them out
    var stem: [String: String] = [:]    // shown name -> its file's stem on this device
    var names: [String] = []            // effects[], in order: the index for walk
    var walk: [String: Bool] = [:]
    var byHash = false                  // compared by content (GET /api/lua/source), else by size
    var fields: [String: String] {
        var f = ["mode": byHash ? "hash" : "size"]
        for (n, b) in bytes { f["s." + n] = hash[n].map { "h" + $0 } ?? "\(b)"; f["w." + n] = walk[n] == false ? "0" : "1" }
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
    // "tz"), the owner's override (climateHa).
    static let exportSkip: Set<String> = ["deviceName", "weatherApiKey", "metricNames", "clockStyle", "timezoneString", "gmtOffset",
                                          "daylightSaving", "climateHa", "ntpServer1", "ntpServer2"]   // NTP: the network's, not carried
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
    struct Summary {
        var id = "", sig = "", panel = "", twin = "", panelFirmware = "", twinFirmware = "", settings: [String] = [], hardware: [String] = []
        var onlyPanel: [String] = [], onlyTwin: [String] = [], differ: [String] = [], effectsKnown = true, bySize = false
        var panelScreen = "", twinScreen = "", why: Msg?, pressReturnForTesting = false
    }
    /// The second window when sync resumes with a pair it knows: what each side changed while it was off.
    struct ResumeSummary { var id = "", panel = "", twin = "", settings: [String] = [], effects: [Msg] = [], conflicts: [String] = [],
                           hardware: [String] = [], panelSettings: [String] = [], panelEffects: [Msg] = [], pressReturnForTesting = false }
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
    // A script's SHA-256 is read again after this long (or when its size changes): the panel's every 5 min,
    // since each read holds its loop(); the twin's every round. Our choice.
    static let hashAge: [Side: TimeInterval] = [.panel: 300, .twin: 20]
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
    /// sync.log is started again past this size, the old one kept as sync.log.1. Our choice.
    static let logKeep = 4 << 20

    let dataDir: URL
    var logFile: URL { dataDir.appendingPathComponent("sync.log") }
    var stateFile: URL { dataDir.appendingPathComponent("sync-state.json") }

    // Shared with the main thread, under the lock.
    private let lock = NSLock()
    private var _enabled = false, _twinAddress: String?, _twinMac = "", _found: [PanelFinder.Found] = []
    private var _lang = "en", _syncNow = false, _reset = false
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
    func setEnabled(_ on: Bool) { locked { _enabled = on; if !on { _reset = true } } }
    func setTwin(address: String?, mac: String) { locked { _twinAddress = address; _twinMac = normMac(mac) } }
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
    /// consented: the first window was answered Enable for this pair since the switch went on; aligned: the
    /// second one too, and the sides were aligned. Nothing is written before both.
    private var consented = false, aligned = false, resumed = false, needDirection = false
    private var question: Question?
    /// An answer that waits for the effects of both sides to be known before it is checked and done.
    private var heldReply: (id: String, reply: Reply)?
    private var alignRetry = Date.distantPast, fxWaitSince: Date?
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
    private var docs: [Side: [String: J]] = [:]
    private var fw: [Side: Firmware] = [:]
    private var caps: [Side: Caps] = [:]
    private var hashes: [Side: [String: (bytes: Int, hash: String, at: Date)]] = [:]
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
    /// A side restarted: the carousel's settings wait for the settings round (fastRound). The twin restarted:
    /// that round takes what differs on it for writes it lost (settingsChanges).
    private var walkHeld = false, twinRestarted = false
    /// A person at the twin (sawTwin): when a touch of its carousel was seen that sync did not make - cleared
    /// by sync's next write there; the twin's last hold read, and when; when sync last wrote to it (a page or a
    /// style holds its carousel, and so does a world clock's new home: web_panel.cpp:764-771).
    private var twinTouch: Date?, twinHoldSeen: (hold: Int, at: Date, on: Bool)?, twinWroteAt = Date.distantPast, personSaid = false

    init(dataDir: URL) { self.dataDir = dataDir }

    func start() {
        guard thread == nil else { return }
        let t = Thread { [weak self] in
            while let self {
                self.step()
                Thread.sleep(forTimeInterval: self.pause)
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
    private func log(_ dir: String, _ what: String, _ m: Msg, problem: Bool = false, sticky: Bool = false) {
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH:mm:ss.SSS"
        let line = "\(f.string(from: Date())) \(dir) \(what) \(T(m))\n"
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
    /// A standing condition: logged once (until sync starts again).
    private func noteOnce(_ key: String, _ m: Msg) { if noted.insert(key).inserted { log("note", "-", m, problem: true, sticky: true) } }

    private func describe(_ e: Error) -> Msg {
        switch e {
        case let s as SyncError: return s.msg
        case let d as SyncDown: return d.msg
        case let w as SyncWait: return w.msg
        case is SyncStopped: return M("sync was switched off", "синхронизацию выключили")
        case let f as Failure: return M(f.description, f.description)
        default: return M(e.localizedDescription, e.localizedDescription)
        }
    }

    // MARK: the loop

    private func step() {
        let (on, reset) = locked { () -> (Bool, Bool) in let r = _reset; _reset = false; return (_enabled, r) }
        if reset || !on {
            if !dev.isEmpty || consented || aligned || question != nil || offer != nil {
                if !on && (consented || aligned || question != nil) {
                    log("note", "off", aligned ? M("sync switched off: nothing more is written; switching it on asks both questions again",
                                                   "синхронизацию выключили: больше ничего не пишу; при включении снова задам оба вопроса")
                                               : M("sync switched off before both questions were answered: nothing was written",
                                                   "синхронизацию выключили до ответа на оба вопроса: ничего не записано"))
                }
                forget()
            }
        }
        guard on else { phase(M("off", "выключена")); return }
        stopLogged = false
        do {
            try connect()
            try verifyIfDue()
            // The owner's two confirmations: until both are given, nothing is written.
            guard consented else { try consentFirst(); return }
            guard aligned else { try align(); return }
            if let (id, go) = locked({ () -> (String, Bool)? in let a = _fwReply; _fwReply = nil; return a }) { try answeredFirmware(id: id, go: go) }
            let now = Date()
            let forced = locked { () -> Bool in let f = _syncNow; _syncNow = false; return f }
            if forced { declined = []; carry = (0, .distantPast); retryPending() }
            let strained = strainedUntil.map { now < $0 } ?? false
            if now >= nextFast || forced {
                try fastRound(); nextFast = Date().addingTimeInterval(strained ? SyncEngine.strainedFastEvery : SyncEngine.fastEvery)
                planStep()
            } else if let at = stepAt, now >= at {
                try stepRead()
            }
            guard aligned else { return }
            // The leader's next step is near: the rounds that take a while (effects, settings) wait until it
            // is carried, stepQuietMax at most - with a short slot a step is always near.
            let near = !forced && (stepAt.map { $0.timeIntervalSince(now) < SyncEngine.stepQuiet } ?? false)
            quietSince = near ? (quietSince ?? now) : nil
            let quiet = near && now.timeIntervalSince(quietSince ?? now) < SyncEngine.stepQuietMax
            if (fxDue || forced || now >= nextEffects) && !quiet {
                fxDue = false
                try effectsRound(); nextEffects = Date().addingTimeInterval(SyncEngine.effectsEvery)
            }
            guard aligned else { return }
            if (forced || now >= nextSlow) && !quiet {
                fwWatch = nil
                try slowRound(market: forced || slowRounds % 5 == 0); slowRounds += 1
                nextSlow = Date().addingTimeInterval(settingsEvery)
            } else if let w = fwWatch, now >= w {
                fwWatch = Date().addingTimeInterval(15)                  // a new image: its /api/info only, again if this fails
                for s in sides { try readDocs(s, ["/api/info"]) }
                fwWatch = nil
                firmwareRound()
            }
            guard aligned else { return }
            try firmwareDue()
            saveState()
            phase(M("on", "включена"))
            if locked({ _status.problem != nil && !_status.sticky }) { publish { $0.problem = nil } }
        } catch is SyncStopped {
            if !stopLogged { stopLogged = true; log("note", "stop", M("sync was switched off: writing stopped", "синхронизацию выключили: запись остановлена")) }
        } catch let w as SyncWait {
            phase(w.msg)
            Thread.sleep(forTimeInterval: 2)
        } catch let d as SyncDown {
            if down.insert(d.side).inserted { log("error", "down", d.msg, problem: true) }
            verifyNow = true
            phase(d.msg)
            if d.side == .panel, (UserDefaults.standard.string(forKey: "panelAddress") ?? "").trimmingCharacters(in: .whitespaces).isEmpty,
               Date().timeIntervalSince(panelLostAt) > 30 {
                panelLostAt = Date()                                     // looked for again over mDNS, at most every 30 s
                DispatchQueue.main.async { self.onPanelLost?() }
            }
            nextFast = Date().addingTimeInterval(5)
            Thread.sleep(forTimeInterval: 3)
        } catch {
            log("error", "-", describe(error), problem: true)
            nextFast = Date().addingTimeInterval(5)
            Thread.sleep(forTimeInterval: 3)
        }
    }

    /// How long the worker sleeps between two steps: until the next screen round or the read that looks for
    /// the leader's carousel step, 0.5 s at most.
    private var pause: TimeInterval {
        guard aligned else { return 0.5 }
        var next = nextFast
        if let s = stepAt, s < next { next = s }
        return min(0.5, max(0.02, next.timeIntervalSinceNow))
    }

    private var settingsEvery: TimeInterval {
        let s = UserDefaults.standard.integer(forKey: "syncSettingsEveryS")
        return TimeInterval(s <= 0 ? 60 : max(15, s))
    }
    private var repo: String { GitHub.repo }

    private func forget() {
        saveState()
        dev = [:]; pairKey = ""; consented = false; aligned = false; resumed = false; needDirection = false; question = nil; holdWhy = nil
        heldReply = nil; alignRetry = .distantPast; fxWaitSince = nil; fxAbsent = [:]; fxAlignFrom = nil; imageCache = nil
        base = [:]; firmwareBase = [:]; screen = [:]; screenAt = [:]; pageReadAt = [:]; wrote = [:]; effects = [:]; docs = [:]; fw = [:]; caps = [:]; hashes = [:]
        stepWin = nil; stepAt = nil; stepTries = 0; quietSince = nil; walkHeld = false; twinRestarted = false; twinTouch = nil; twinHoldSeen = nil; twinWroteAt = .distantPast; personSaid = false
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
            d.willWrite = { [weak self] in self?.twinWroteAt = Date() }
            dev[.twin] = d; fw[.twin] = f
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
            dev[.panel] = d; fw[.panel] = f
            publish { $0.peer = "\(f.name) (\(pAddr), \(f.mac))" }
        }
        let key = "\(fw[.panel]!.mac)|\(fw[.twin]!.mac)"
        if key != pairKey {
            base = [:]; firmwareBase = [:]; pendingFx = [:]; noted = []; resumed = false; needDirection = false; question = nil; wrote = [:]
            stepWin = nil; stepAt = nil; stepTries = 0; twinTouch = nil; twinHoldSeen = nil
            wantPanel = nil; wantTwin = nil; inFlight = nil; heldReply = nil; fxAlignFrom = nil; imageCache = nil
            pairKey = key; consented = false; aligned = false; loadState()
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
        guard let pn = try d.get("/api/panel") else {
            throw SyncError(M("\(s.word.en) has no /api/panel: its firmware is too old for sync", "\(s.at) нет /api/panel: прошивка слишком старая для синхронизации"))
        }
        let got = Date()
        guard let st = try d.get("/api/status") else {
            throw SyncError(M("\(s.word.en) has no /api/status: its firmware is too old for sync", "\(s.at) нет /api/status: прошивка слишком старая для синхронизации"))
        }
        var x = Screen()
        x.apply(panel: pn)
        x.bright = st.i("brightness") ?? 0; x.off = st.b("forcedOff") ?? false; x.uptime = st.i("uptime") ?? 0
        docs[s, default: [:]]["/api/panel"] = pn; pageReadAt[s] = got
        if s == .panel { timeStep(x, sent: sent, received: got) }
        else {
            // Restarted (a smaller uptime): its carousel's hold starts from boot - no touch (rebooted() follows).
            if let prev = screen[.twin], x.uptime < prev.uptime { twinTouch = nil; twinHoldSeen = nil }
            sawTwin(x, at: got)
        }
        return x
    }

    /// The uploaded scripts; nil when they cannot be known now - a build without Lua (404) or without the
    /// script store (no "uploaded"), a filesystem that did not mount (count 0 and fsFree 0:
    /// luaStoreFreeBytes, lua_store.cpp:126), or whether the scripts can be read is not known yet.
    /// Unknown is never taken for "no effects".
    private func readEffects(_ s: Side) throws -> Effects? {
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
            e.bytes[shownName(stem)] = b; e.stem[shownName(stem)] = stem
        }
        guard let byHash = luaCap(s) else { return nil }
        e.byHash = byHash
        if byHash { for (n, stem) in e.stem { e.hash[n] = try scriptHash(s, stem: stem, bytes: e.bytes[n]!) } }
        return e
    }

    /// A script's SHA-256 (16 hex digits), read again when its size changed or every hashAge.
    private func scriptHash(_ s: Side, stem: String, bytes: Int) throws -> String {
        if let c = hashes[s]?[stem], c.bytes == bytes, Date().timeIntervalSince(c.at) < SyncEngine.hashAge[s]! { return c.hash }
        let d = try device(s).luaSource(stem)
        guard d.count == bytes else { throw SyncError(M("\(stem): \(d.count) bytes read, the list says \(bytes)", "\(stem): прочитано \(d.count) байт, в списке \(bytes)")) }
        let h = String(sha256Hex(d).prefix(16))
        hashes[s, default: [:]][stem] = (bytes, h, Date())
        return h
    }

    private static let settingsRoutes = ["/api/info", "/api/portal", "/api/export", "/api/panel", "/api/knob", "/api/worldclock",
                                         "/api/railboard", "/api/flightboard", "/api/media", "/api/ir"]

    private func readDocs(_ s: Side, _ routes: [String]) throws {
        var d = docs[s] ?? [:]
        for r in routes {
            if let doc = try device(s).get(r) { d[r] = doc } else { d[r] = nil }
            if r == "/api/info", let doc = d[r] { fw[s] = Firmware(doc) }
        }
        docs[s] = d
    }

    private func flat(_ s: Side) -> J { Settings.flat(docs[s] ?? [:]) }
    private func digests(_ f: J) -> [String: String] { f.mapValues { digest($0) } }

    private func readAll() throws {
        for s in sides {
            screen[s] = try readScreen(s); screenAt[s] = Date()
            try readDocs(s, SyncEngine.settingsRoutes + ["/api/market"])
            effects[s] = try readEffects(s)
        }
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
        let waited = fxWaitSince != nil
        if waited { for s in sides where effects[s] == nil && fxAbsent[s] == false { effects[s] = try readEffects(s) } }
        else { try readAll() }
        guard effectsReady() else { throw SyncWait(SyncEngine.waitingForEffects) }
        if waited { try readAll() }
        if resumed && !needDirection {
            if let why = resumeNeedsDirection() { needDirection = true; holdWhy = why; askDirectionNow(why) } else { try resume() }
        } else { askDirectionNow(holdWhy) }
        if !aligned { throw SyncWait(M("waiting for your answer", "жду ответа")) }
    }

    /// The answer, done for exactly what was shown: both sides are read again first - after the effects
    /// of both, if a side's are not known just now (the answer is held until they are, fxWaitMax at most) -
    /// and if anything the question listed changed meanwhile, it is asked again with the fresh list.
    private func answered(_ q: Question, _ r: Reply) throws {
        try readAll()
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
        if let a = effects[from], let b = effects[to] { try convergeEffects(from: from, a, b) }
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
    /// was off, whatever that is - even nothing.
    private func resume() throws {
        let plan = resumePlan()
        if let m = plan.mass {
            needDirection = true; holdWhy = m
            log("note", "hold", M("\(m.en): which way?", "\(m.ru): в какую сторону?"), problem: true); askDirectionNow(m); return
        }
        askResumeNow(plan)
    }

    /// "From the twin to the panel" at a resume: the changes of both sides since the saved bases, as the
    /// rounds would carry them, with no threshold - the person has seen the list. Effects that a side's
    /// firmware cannot give out are left without a base: the direction is asked the day they can be read.
    private func carryResume() throws {
        log("twin→panel", "resume", M("the changes made while sync was off: the twin's go to the panel, the panel's to the twin",
                                      "изменения, сделанные без синхронизации: двойника переношу на панель, панели — на двойника"))
        try settingsPass(allowMass: true)
        if let p = effects[.panel], let t = effects[.twin] { try effectsPass([.panel: p, .twin: t], allowMass: true) }
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
        guard aligned, SyncEngine.leader(panel: screen[.panel]) == .panel, !personAtTwin, let w = stepWin,
              stepTries < SyncEngine.STEP_TRIES else { return }
        stepAt = SyncEngine.stepReadAt((w.lo, w.hi))
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
                rebaseScreen(.panel)                                      // its walk: nobody's change
                try twinAct(quick: true)
                nextFast = max(nextFast, Date().addingTimeInterval(SyncEngine.fastEvery))
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
        t.apply(panel: tp)
        docs[.twin, default: [:]]["/api/panel"] = tp
        screen[.twin] = t; pageReadAt[.twin] = Date()
        sawTwin(t, at: Date())
        return personAtTwin || !SyncEngine.screenChanges(t, base: b, wrote: wrote[.twin]).changed.isDisjoint(with: ["page", "style"])
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
            catch { if error is SyncStopped || error is SyncDown || tryAgain("follow", .panel, Array(f)) { throw error } }
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
        for s in sides { fresh[s] = try readScreen(s) }
        // A reboot, or an uptime that jumped ahead: the pair is checked before anything is written.
        var reboots: [Side] = [], jumped = false
        for s in sides {
            guard let prev = screen[s], let at = screenAt[s] else { continue }
            let x = fresh[s]!
            if prev.luaSig != x.luaSig { fxDue = true }
            if x.uptime < prev.uptime { reboots.append(s) }
            else if x.uptime > prev.uptime + Int(Date().timeIntervalSince(at)) + 30 { jumped = true }   // 30 s of slack: our choice
        }
        if !reboots.isEmpty || jumped { try verifyIdentity() }
        for s in sides { screen[s] = fresh[s]; screenAt[s] = Date() }
        for s in reboots { rebooted(s, fresh[s]!) }
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
        }
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
        for s in sides {
            let c = changed[s]!
            if !c.isEmpty {
                do { try mirrorScreen(from: s, fields: c); forgive("screen", s, Array(c)) }
                catch { if error is SyncStopped || error is SyncDown || tryAgain("screen", s, Array(c)) { throw error } }   // the base stays: seen again
            }
            rebaseScreen(s)
        }
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
        let transient = error is SyncDown || error is SyncStopped || error is SyncWait
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
        if s == .twin { twinTouch = nil; twinHoldSeen = nil; twinRestarted = true }   // its carousel starts again
        walkHeld = true; nextSlow = Date()                           // the settings round first (fastRound)
        fwWatch = Date().addingTimeInterval(5)
        caps[s] = nil
        if s == .twin, aligned { twinFollowsPanel() }
    }

    /// The twin takes the panel's screen (after the twin restarted, after a firmware sync sent it was confirmed).
    /// A failure is said, not thrown: the twin keeps its own screen until the next change on either side.
    private func twinFollowsPanel() {
        do { try mirrorScreen(from: .panel, fields: SyncEngine.screenFields) }
        catch is SyncStopped {}
        catch { log("note", "screen", M("the twin did not take the panel's screen: " + describe(error).en, "двойник не принял экран панели: " + describe(error).ru)) }
    }

    /// Makes the other side's screen show FIELDS of side S's, then reads it again as its base. The panel's
    /// carousel walk is written only while the panel leads (leader), the twin's never; what is written is kept
    /// in `wrote` until it is read back. QUICK (the panel's step, page and style only): the other side's
    /// /api/panel read within fastEvery + 1 s stands for a read before the write - its page numbers - and the
    /// answer to the write, the same document as GET /api/panel, for the read after it.
    private func mirrorScreen(from s: Side, fields asked: Set<String>, quick: Bool = false) throws {
        let o = s.other
        guard let src = screen[s] else { return }
        let leads = SyncEngine.leader(panel: screen[.panel]) == s
        let fields = SyncEngine.writable(asked, from: s, src, panel: screen[.panel])
        guard !fields.isEmpty else { return }
        let fresh = quick && fields.isSubset(of: ["page", "style"]) && pageReadAt[o].map { Date().timeIntervalSince($0) < SyncEngine.fastEvery + 1 } == true
        var dst = fresh ? screen[o]! : try readScreen(o)
        var body: J = [:], did: [String] = [], expect: [String: String] = [:]
        if fields.contains("page"), src.shown != dst.shown {
            if src.key == "cards" {
            } else if let i = dst.index(key: src.key, name: src.name) {
                body["show"] = ["page": i]; expect["page"] = src.shown; did.append(M("page \(src.name)", "страница \(src.name)").text(lang))
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
            let f = cur[s]!.fields, b = base[s]?["effects"] ?? [:]
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
        for s in sides { r.deletes[s] = r.changed[s]!.filter { $0.hasPrefix("s.") && cur[s]!.fields[$0] == nil }.count }
        return r
    }

    private func effectsRound() throws {
        var cur: [Side: Effects] = [:]
        for s in sides {
            guard let e = try readEffects(s) else {
                if fxAbsent[s] == true {
                    noteOnce("fx-absent-\(s)", M("the effects of \(s.word.en) cannot be read with its firmware (no /api/lua, no script store or no filesystem): not compared",
                                                 "эффекты \(s.of) не прочитать с этой прошивкой (нет /api/lua, хранилища скриптов или файловой системы): не сравниваю"))
                } else {
                    noteOnce("fx-unknown-\(s)", M("the effects of \(s.word.en) cannot be read just now (/api/lua/source did not answer): compared when they can be",
                                                  "эффекты \(s.of) сейчас не прочитать (/api/lua/source не ответил): сравню, когда прочитаются"))
                }
                return
            }
            cur[s] = e
        }
        effects = cur
        for s in sides where !cur[s]!.byHash {
            noteOnce("fx-size-\(s)-\(fw[s]?.id ?? "")", M("the effects of \(s.word.en) are compared by size: its firmware has no /api/lua/source, so an edit that keeps a script's length is not seen",
                                                         "эффекты \(s.of) сравниваются по размеру: в прошивке \(s.of) нет /api/lua/source, и правку, не меняющую длину скрипта, не видно"))
        }
        try effectsPass(cur, allowMass: false)
    }

    private func effectsPass(_ cur: [Side: Effects], allowMass: Bool) throws {
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
        for s in sides { base[s, default: [:]]["effects"] = cur[s]!.fields }
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
        catch let e where e is SyncStopped || e is SyncDown || e is SyncWait { throw e }
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
    }

    private func convergeEffects(from s: Side, _ a: Effects, _ b: Effects) throws {
        var keys: [String] = []
        for n in Set(a.bytes.keys).union(b.bytes.keys) where !Effects.same(a, b, n) { keys.append("s." + n) }
        for n in a.bytes.keys where b.bytes[n] != nil && (a.walk[n] ?? true) != (b.walk[n] ?? true) { keys.append("w." + n) }
        if !keys.isEmpty { try applyEffects(from: s, keys: keys.sorted()) }
    }

    /// Carries effect changes of side S to the other side: removals, then new and replaced scripts, then walk switches.
    private func applyEffects(from s: Side, keys: [String]) throws {
        let o = s.other
        guard let src = effects[s], var dst = try readEffects(o) else {
            throw SyncError(M("the effects of \(o.word.en) cannot be read now", "эффекты \(o.of) сейчас не прочитать"))
        }
        var wrote = false
        func again() throws -> Effects {
            guard let e = try readEffects(o) else { throw SyncError(M("the effects of \(o.word.en) cannot be read now", "эффекты \(o.of) сейчас не прочитать")) }
            return e
        }
        for k in keys where k.hasPrefix("s.") && src.bytes[String(k.dropFirst(2))] == nil {
            let name = String(k.dropFirst(2))
            pendingFx[s]?[name] = nil
            guard let stem = dst.stem[name] else { continue }
            try device(o).post("/api/lua", ["delete": stem])                  // puts the clock on (web_panel.cpp:1004-1005)
            hashes[o]?[stem] = nil
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
            guard let bytes = src.bytes[name], let stem = src.stem[name], !knownSame(name) else { continue }
            guard let script = try script(from: s, name: name, stem: stem, bytes: bytes, hash: src.hash[name]) else {
                pendingFx[s, default: [:]][name] = bytes
                noteOnce("fx-\(s)-\(name)-\(bytes)", M("\(name) is not carried to \(o.word.en): the firmware of \(s.word.en) has no /api/lua/source, and the gallery of \(repo) has no such file",
                                                        "\(name) не перенести на \(o.to): в прошивке \(s.of) нет /api/lua/source, а в галерее \(repo) такого файла нет"))
                continue
            }
            let target = dst.stem[name] ?? stem                               // the same effect keeps the target's file name
            try uploadEffect(o, stem: target, data: script)
            hashes[o]?[target] = nil
            pendingFx[s]?[name] = nil
            wrote = true
            log(s.arrow, "effect", M("\(dst.bytes[name] == nil ? "new" : "replaced") \(name) (\(bytes) B)",
                                     "\(dst.bytes[name] == nil ? "новый" : "заменён") эффект \(name) (\(bytes) Б)"))
            dst = try again()
            if src.walk[name] == false, dst.walk[name] != false, let i = dst.names.firstIndex(of: name) {
                try device(o).post("/api/lua", ["walk": ["i": i, "on": false, "name": name] as J])
                dst = try again()
            }
        }
        for k in keys where k.hasPrefix("w.") {
            let name = String(k.dropFirst(2))
            guard let want = src.walk[name], dst.walk[name] != nil, dst.walk[name] != want, let i = dst.names.firstIndex(of: name) else { continue }
            try device(o).post("/api/lua", ["walk": ["i": i, "on": want, "name": name] as J])   // 409 if the list moved meanwhile
            wrote = true
            log(s.arrow, "effect", M("\(name) \(want ? "in" : "out of") the walk", "\(name) \(want ? "в обходе" : "вне обхода")"))
        }
        guard wrote else { return }
        effects[o] = try again()
        base[o, default: [:]]["effects"] = effects[o]!.fields
        try afterWrite(o, from: s)
    }

    private func uploadEffect(_ o: Side, stem: String, data: Data) throws {
        for attempt in 1...4 {
            // The upload runs a trial of the script inside loop(): up to 15 s (lua_effects.cpp:572-600).
            let a = try device(o).upload("/api/lua/upload", query: [("name", stem)], field: "script", filename: "\(stem).lua", data: data, timeout: 90)
            if a.status == 200, a.object?.b("success") == true { return }
            if a.why.contains("another upload"), attempt < 4 { Thread.sleep(forTimeInterval: 2); continue }
            throw device(o).refused("POST /api/lua/upload?name=\(stem)", a)
        }
    }

    /// The script's bytes: from the device itself where it has /api/lua/source, else the gallery file with
    /// the same name and size (and SHA-256, where the index gives one); nil when neither has it.
    private func script(from s: Side, name: String, stem: String, bytes: Int, hash: String?) throws -> Data? {
        switch luaCap(s) {
        case true?:
            let d = try device(s).luaSource(stem)
            guard d.count == bytes, hash == nil || sha256Hex(d).hasPrefix(hash!) else {
                throw SyncError(M("\(name) changed while it was read: carried next time", "\(name) изменился, пока читался: перенесу в следующий раз"))
            }
            return d
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
    /// to the twin (RESIDUE, among the panel's). TWIN_RESTARTED: the twin restarted since the last full pass,
    /// and what differs on it from its base is no person's change but a write it lost - each module saves its
    /// settings to NVS a moment later (panel.cpp panelTick, SETTLE_MS 2.5 s), and a restart within that moment
    /// keeps the old value: the panel's value goes back to the twin (RESTORED, among the panel's). ONLY: these
    /// keys alone.
    static func settingsChanges(cur: [Side: J], dig: [Side: [String: String]], base bp: [String: String], _ bt: [String: String],
                                only: ((String) -> Bool)? = nil, twinRestarted: Bool = false)
        -> (changed: [Side: [String]], conflicts: [String], hardware: [String], residue: [String], restored: [String]) {
        var changed: [Side: [String]] = [:]
        for (s, b) in [(Side.panel, bp), (.twin, bt)] {
            changed[s] = dig[s]!.keys.filter { (only?($0) ?? true) && b[$0] != nil && b[$0] != dig[s]![$0] }.sorted()
        }
        var restored: [String] = []
        if twinRestarted {
            restored = changed[.twin]!.filter { !changed[.panel]!.contains($0) }
            changed[.panel] = Array(Set(changed[.panel]!).union(changed[.twin]!)).sorted()
            changed[.twin] = []
        }
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
        // The twin's restart is judged by the first full pass after it (walkHeld keeps the screen round's
        // carousel pass waiting for it).
        let restarted = only == nil && twinRestarted
        if only == nil { twinRestarted = false }
        let ch = SyncEngine.settingsChanges(cur: cur, dig: dig, base: bp, bt, only: only, twinRestarted: restarted)
        let changed = ch.changed
        for k in ch.conflicts { log("panel→twin", "conflict", M("conflict: the panel's taken - \(Settings.shown(k))", "конфликт: взята панель — \(Settings.shown(k))")) }
        // The panel's hardware never goes to the panel: the twin keeps its own until the panel changes it.
        for k in ch.hardware {
            noteOnce("hw-\(k)-\(dig[.twin]![k] ?? "")", M("\(Settings.shown(k)): the panel's hardware setting - not carried from the twin to the panel",
                                                          "\(Settings.shown(k)): настройка железа панели — с двойника на панель не переношу"))
        }
        if !ch.residue.isEmpty {
            log("override", "twin", M("no longer overridden on the twin (the owner, 2026-09-30 19:35): the panel's \(ch.residue.map(Settings.shown).joined(separator: ", "))",
                                      "переопределения двойника сняты (владелец, 30.09 19:35): беру с панели \(ch.residue.map(Settings.shown).joined(separator: ", "))"))
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

    /// The owner's override on the twin (sync.py OVERRIDES): climateHa off, put back whenever it drifts; it is
    /// never compared or sent to the panel. And what the dropped overrides of 2026-09-29 added to the twin: the
    /// custom airport ZZZZ "NO REQUESTS", removed once the twin's board is on another airport (the panel's,
    /// settingsPass) - unless the panel has such an airport itself, or its list is not known. Neither touches
    /// a compared setting (ZZZZ is left out of flightboard.custom), so no base moves.
    private func enforceOverrides() throws {
        let tw = device(.twin), d = docs[.twin] ?? [:]
        var did: [String] = [], routes: [String] = []
        if d["/api/export"]?.b("climateHa") == true {
            try tw.post("/api/import", ["climateHa": false]); did.append("climateHa false")    // climate.cpp:127-131, 165-182
            routes += ["/api/export", "/api/portal"]
        }
        if let fb = d["/api/flightboard"],
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

    /// After writing to side O: its screen as it is now becomes its base (a new home city, a removed
    /// effect or a renumbered list can move its page); if its page had been the source's and moved,
    /// the source's page is put back.
    private func afterWrite(_ o: Side, from s: Side) throws {
        let before = screen[o]
        guard let now = try? readScreen(o) else { return }
        screen[o] = now; screenAt[o] = Date(); rebaseScreen(o)
        if let before, let src = screen[s], before.shown == src.shown, now.shown != src.shown {
            try mirrorScreen(from: s, fields: ["page"])
        }
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
            caps[s] = nil; hashes[s] = [:]; imageCache = nil
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
            caps[s] = nil; hashes[s] = [:]; stateDirty = true
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
                catch let e where e is SyncStopped || e is SyncWait { throw e }
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
        catch let e where e is SyncStopped || e is SyncWait { throw e }
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
        var v: Int?                                   // 2: settings as HMAC digests under StateKey
        var key: String?                              // StateKey.tag of the key they were made with
        var pair: String
        var settings: [String: [String: String]]
        var effects: [String: [String: String]]
        var firmware: [String: String]
        var pending: [String: [String: Int]]?        // effects not carried yet, by the side that has them
        var wantPanel: String?, wantTwin: String?     // a firmware one side should get
        var inFlight: InFlight?
    }

    private func saveState() {
        guard StateKey.kept, aligned, !pairKey.isEmpty else { return }
        var s = Saved(v: 2, key: StateKey.tag, pair: pairKey, settings: [:], effects: [:], firmware: [:], pending: [:],
                      wantPanel: wantPanel, wantTwin: wantTwin, inFlight: inFlight)
        for side in sides {
            s.settings[side.rawValue] = base[side]?["settings"] ?? [:]
            s.effects[side.rawValue] = base[side]?["effects"] ?? [:]
            s.firmware[side.rawValue] = firmwareBase[side] ?? ""
            s.pending?[side.rawValue] = pendingFx[side] ?? [:]
        }
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]; enc.dateEncodingStrategy = .iso8601
        if let d = try? enc.encode(s), d != lastSaved { try? d.write(to: stateFile, options: .atomic); lastSaved = d }
        stateDirty = false
    }

    private func loadState() {
        guard StateKey.kept else {
            noteOnce("no-key", M("the Keychain does not give the sync state's key: nothing is kept between runs, and each start asks the direction",
                                 "Связка ключей не отдаёт ключ состояния синхронизации: между запусками ничего не сохраняется, и при каждом запуске спрошу направление"))
            return
        }
        let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
        guard let d = try? Data(contentsOf: stateFile), let s = try? dec.decode(Saved.self, from: d), s.pair == pairKey else { return }
        guard s.v == 2, s.key == StateKey.tag else {
            log("note", "state", M("sync-state.json was written by an earlier version or with another key: not used - the direction is asked",
                                   "sync-state.json записан прежней версией или другим ключом: не использую — спрошу направление"))
            return
        }
        for side in sides {
            base[side, default: [:]]["settings"] = s.settings[side.rawValue] ?? [:]
            base[side, default: [:]]["effects"] = s.effects[side.rawValue] ?? [:]
            if let f = s.firmware[side.rawValue], !f.isEmpty { firmwareBase[side] = f }
            if let p = s.pending?[side.rawValue], !p.isEmpty { pendingFx[side] = p }
        }
        wantPanel = s.wantPanel; wantTwin = s.wantTwin; inFlight = s.inFlight
        resumed = !(s.settings["panel"] ?? [:]).isEmpty
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
