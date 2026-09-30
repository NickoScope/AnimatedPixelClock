// SyncEngine.swift - Sync with panel: the app's twin and one physical panel kept as one device.
//
// The owner, 2026-09-30 09:15: while the switch is on, the screen (page, effect, clock style,
// brightness, on/off), the settings, the Lua effects (a new one appears, a removed one goes) and the
// firmware are mirrored both ways. Firmware goes to the twin by itself and to the physical panel only
// after a person says yes here ("Update the panel too?"). What sync brings is not sent back (no echo);
// when both sides change the same thing, the later change wins. The owner's overrides on the twin
// (2026-09-29, tools/twin/sync.py OVERRIDES) hold always and never travel to the panel; secrets,
// identity and state never travel at all. Other owners work in their own forks: firmwareRepo names
// where releases and the gallery come from.
//
// The firmware has no push channel - no WebSocket, no event stream, no MQTT topic with the screen's
// state - so both devices are polled over HTTP, one request at a time: the firmware's WebServer serves
// a request inside loop() and the panel stands still meanwhile (web_panel.cpp:1066-1068), and a burst
// of parallel requests once drained the radio's memory (web_heap_backoff.h:4-12). Every request here
// comes from one worker thread, and requests to the panel are paced. 503 with Retry-After is the
// firmware standing aside - nothing was done - and is asked again (web.cpp:1421-1456).
//
// What is read, and how often (the periods are our choice, not measured; the portal itself polls
// /api/panel every 2 s and /api/info every 5 s while it is open, web_panel_js.h:86, web_pages.h:2376):
//   screen    every 3 s   GET /api/panel (web_panel.cpp:295-377: now.key/name/style, carousel.running,
//                         pageS, pages[]) and GET /api/status (web.cpp:554-580: brightness %, forcedOff,
//                         uptime - a smaller uptime is a reboot, whose reset screen is nobody's change)
//   effects   when the Lua names or walk switches in /api/panel change, and every 60 s: GET /api/lua
//                         (web_panel.cpp:968-1062; slow, about 0.5 s on the twin)
//   settings  every syncSettingsEveryS (60 s): /api/info, /api/portal, /api/export, /api/knob,
//             /api/worldclock, /api/railboard, /api/flightboard, /api/media, /api/ir; /api/market (30 KB)
//             every fifth time. /api/info also carries the firmware: version, build, firmwareBytes,
//             ota.state (web.cpp:431-551), read again every 15 s after a reboot until it is "valid".
//
// Change, echo, who wins. Each side has a base: what it held after the last round, with what this
// engine wrote to it included - after every write the target is read again and that reading becomes
// its base, so a change that arrived by sync is never taken for the person's and sent back; and
// nothing is written that the target already holds. A change is a field whose value differs from its
// side's base. When both sides changed one field in one round, the page goes to the side whose
// carousel.pageS is smaller - the firmware's only clock of changes, reset by every show, knob turn and
// remote press (carousel.cpp:13-17); for everything else there is no timestamp on the device, so the
// order is known only to within a poll period, and a tie goes to the panel. A page or style change
// made by the carousel itself (carousel.running, carousel.cpp:25-27) is not mirrored: two carousels
// would fight. The bases of settings, effects and firmware are kept (as digests, no values) in
// <dataDir>/sync-state.json, so a restart of the app resumes where it stopped: what changed on either
// side meanwhile is mirrored as usual. The first time a pair is synced, the person picks the direction.
//
// How each thing is written:
//   page      POST /api/panel {"show":{"page":i}}, i looked up by key and name on the target
//             (web_panel.cpp:385-398; indexes differ per device, main.cpp:95-110)
//   style     POST /api/panel {"style":id,"show":{"page":i}}: style is checked and stored 2.5 s later,
//             and moves to the clock page, so show keeps the page (web_panel.cpp:400-402, 465-466)
//   brightness GET /api/display/brightness?value=0..100 - percent, exact both ways; 0 is off
//             (web.cpp:596-609, display.cpp:183-195)
//   on / off  GET /api/display/on | off (web.cpp:582-594)
//   settings  the export's keys through POST /api/import, only those that changed (web.cpp:2499-2761);
//             the rest of the portal's form through POST /save, the whole form - the target's own
//             values with the changed ones replaced, an absent checkbox reads as off, the metric rows
//             and colours ride along or are reset (web.cpp:1537-2264); the zone as a region through
//             /save, a zone that is no region through /api/import. The Panel group's routes as
//             tools/twin/sync.py writes them (pages, carousel, knob, world clock, rail board, flight
//             board, market, media), a remote button's function by GET /api/ir/fn, the log by GET
//             /api/log?on= (web.cpp:191-217, 262-270).
//   effects   the source from GET /api/lua/source?name= where the firmware has it (branch
//             feat/sync-routes, web_panel.cpp handleLuaSource), else the gallery file of firmwareRepo
//             with the same name and size (raw.githubusercontent.com/<repo>/main/gallery/index.json);
//             then POST /api/lua/upload?name=<stem>, multipart, no Origin header (web_panel.cpp:945-963,
//             1065-1197). Removed: POST /api/lua {"delete":stem} (web_panel.cpp:994-1012). The walk
//             switch: POST /api/lua {"walk":{"i","on","name"}} (web_panel.cpp:976-993).
//   firmware  the image from GET /api/firmware/image where the firmware has it (feat/sync-routes,
//             web.cpp handleFirmwareImage), else the release of firmwareRepo with that version whose
//             OTA_ONLY image has the same size, checked against SHA256SUMS.txt; always the ESP image's
//             own checks (magic 0xE9, chip id 9, the appended SHA-256). Then POST /update, multipart
//             (web.cpp:336-393), and the target is watched until /api/info says that version and build
//             with ota.state "valid" (boot_health.cpp:60-86), or that it rolled back.
//
// Never written: /reset (a GET wipes everything, web.cpp:182, 2266-2285), /api/reboot, /api/rename,
// deviceName, the network fields, weatherApiKey (only "set" is ever compared, never the key), metric
// names, counters, caches, the remote's learned codes. The owner's overrides on the twin: climateHa off,
// the trains and flights pages out of the walk, the flight board on the custom airport ZZZZ "NO
// REQUESTS" (sync.py OVERRIDES); they are left out of what is compared, put back on the twin whenever
// they drift, and the trains and flights pages are not shown on the twin by sync either.
//
// Safety. The panel is a device on the network that answers /api/info with model AnimatedPixelClock,
// is not a twin (a TWIN- name or a MAC 02:54:57:49:*, the twins' locally administered block,
// tools/twin/twin.py) and is not this app's twin (its MAC, its address). A change of more than
// MASS_SETTINGS settings or MASS_DELETES effect removals at once on the twin is not sent to the panel:
// the person is asked for the direction instead (a twin whose flash was erased looks like that).
//
// Settings (defaults): syncEnabled (off by default; the switch), panelAddress (host[:port]; empty:
// found over mDNS), panelMac (the panel picked from the found ones), firmwareRepo (owner/name, default
// NickoScope/AnimatedPixelClock), syncSettingsEveryS (60, at least 15).
// For tests only, and honoured only when the "panel" is itself a twin - they can never act on a real
// panel: syncPanelMayBeTwinForTesting (accept a twin as the panel), syncAutoConfirmForTesting (answer
// "Update the panel" by itself), syncAlignForTesting ("panel" | "twin": the first direction without
// asking). All off by default.

import AppKit
import CryptoKit
import Foundation

// MARK: - words

/// A message in both languages; rendered when shown, so the EN · RU switch retranslates it.
struct Msg { let en: String, ru: String; func text(_ lang: String) -> String { lang == "ru" ? ru : en } }
func M(_ en: String, _ ru: String) -> Msg { Msg(en: en, ru: ru) }

struct SyncError: Error { let msg: Msg; init(_ m: Msg) { msg = m } }
/// A device does not answer at all: said once, not every round.
struct SyncDown: Error { let side: Side; let msg: Msg }
/// Not an error: the engine is waiting (no panel yet, the twin is starting, a question is open).
struct SyncWait: Error { let msg: Msg; init(_ m: Msg) { msg = m } }

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
/// What a base keeps of a value: its digest - equality is all a base is for, and the file holds no values.
func digest(_ v: Any?) -> String { SHA256.hash(data: Data(canon(v).utf8)).prefix(12).map { String(format: "%02x", $0) }.joined() }
func jsonData(_ o: Any) throws -> Data { try JSONSerialization.data(withJSONObject: o, options: [.withoutEscapingSlashes]) }
func formText(_ v: Any) -> String { (v as? NSNumber)?.stringValue ?? (v as? String) ?? "\(v)" }
/// "my_fx" -> "MY FX": the name the banner shows and the walk switch is kept by (lua_store.cpp:49-53).
func shownName(_ stem: String) -> String { String(stem.map { $0 == "_" ? " " : Character($0.uppercased()) }) }
func normMac(_ m: String?) -> String { (m ?? "").trimmingCharacters(in: .whitespacesAndNewlines).uppercased().replacingOccurrences(of: "-", with: ":") }
/// The twins' block: locally administered, "TWI" (tools/twin/twin.py MAC; main.swift Twin.prepare).
func twinLike(name: String, mac: String) -> Bool { name.hasPrefix("TWIN-") || normMac(mac).hasPrefix("02:54:57:49:") }
/// host or IPv4, with an optional port: nothing that could carry a path or a query onto a device.
func validAddress(_ a: String) -> Bool { a.range(of: #"^[A-Za-z0-9.-]{1,253}(:[0-9]{1,5})?$"#, options: .regularExpression) != nil }
func validRepo(_ r: String) -> Bool { r.range(of: #"^[A-Za-z0-9-]{1,39}/[A-Za-z0-9_.-]{1,100}$"#, options: .regularExpression) != nil }

// MARK: - one device over HTTP

struct Answer {
    let status: Int, data: Data, headers: [String: String]
    var text: String { String(decoding: data, as: UTF8.self) }
    var object: J? { try? JSONSerialization.jsonObject(with: data) as? J }
    func header(_ n: String) -> String? { headers.first { $0.key.caseInsensitiveCompare(n) == .orderedSame }?.value }
    /// The firmware's own words for a refusal: {"success":false,"error":...} or "message".
    var why: String { let o = object; return String((o?.s("error") ?? o?.s("message") ?? text).prefix(160)) }
}

final class Device {
    let side: Side, address: String
    /// A pause after every request to the panel, so polling never runs back to back on it.
    let pace: TimeInterval

    init(_ side: Side, _ address: String) { self.side = side; self.address = address; pace = side == .panel ? 0.15 : 0 }

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

    /// One request. 503 is asked again after Retry-After; a transport fault is retried for GET and
    /// HEAD only - a POST may have landed.
    func send(_ method: String, _ path: String, query: [(String, String)] = [], body: Data? = nil, type: String? = nil,
              timeout: TimeInterval = 15, limit: Int = 2 << 20) throws -> Answer {
        var c = URLComponents(string: "http://\(address)\(path)")!
        if !query.isEmpty { c.queryItems = query.map { URLQueryItem(name: $0.0, value: $0.1) } }
        guard let url = c.url else { throw SyncError(M("bad address \(address)", "неверный адрес \(address)")) }
        var attempt = 0
        while true {
            attempt += 1
            var req = URLRequest(url: url, timeoutInterval: timeout)
            req.httpMethod = method; req.httpBody = body
            if let type { req.setValue(type, forHTTPHeaderField: "Content-Type") }
            let (a, err) = Device.exchange(req, limit: limit)
            if pace > 0 { Thread.sleep(forTimeInterval: pace) }
            if let a, a.status == 503, attempt < 6 {
                Thread.sleep(forTimeInterval: (Double(a.header("Retry-After") ?? "") ?? 1) + 0.4 * Double(attempt)); continue
            }
            if let a { return a }
            if method == "GET" || method == "HEAD", attempt < 3 { Thread.sleep(forTimeInterval: 0.8); continue }
            throw SyncDown(side: side, msg: M("\(side.word.en) at \(address) does not answer (\(err ?? "?"))",
                                              "\(side.word.ru) по адресу \(address) не отвечает (\(err ?? "?"))"))
        }
    }

    static func exchange(_ req: URLRequest, limit: Int) -> (Answer?, String?) {
        let sem = DispatchSemaphore(value: 0)
        var out: (Answer?, String?) = (nil, "no answer")
        session.dataTask(with: req) { d, r, e in
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
        }.resume()
        sem.wait()
        return out
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
    func hasLuaSource() -> Bool { (try? send("GET", "/api/lua/source"))?.status == 400 }
    func hasImageRoute() -> Bool { (try? send("HEAD", "/api/firmware/image"))?.status == 200 }

    func luaSource(_ stem: String) throws -> Data {
        let a = try send("GET", "/api/lua/source", query: [("name", stem)], timeout: 30, limit: 1 << 20)
        guard a.status == 200, a.header("X-Lua-Name") == stem else { throw refused("GET /api/lua/source?name=\(stem)", a) }
        return a.data
    }
    func firmwareImage() throws -> Answer {
        let a = try send("GET", "/api/firmware/image", timeout: 180, limit: 8 << 20)
        guard a.status == 200 else { throw refused("GET /api/firmware/image", a) }
        return a
    }
}

// MARK: - what a device holds

struct Screen {
    var key = "", name = "", page = 0, style = 0, bright = 0, off = false, uptime = 0
    var running = false, allStyles = false, pageS = 0
    var pages: [J] = [], styles: Set<Int> = []
    /// What is mirrored as "the page": its key and name (indexes differ per device). Cards are this
    /// device's own notifications and stay out.
    var shown: String { key == "cards" ? "cards" : "\(key)/\(name)" }
    var fields: [String: String] { ["page": shown, "style": "\(style)", "bright": "\(bright)", "off": off ? "1" : "0"] }
    /// The Lua pages and their walk switches: when this changes, the effects are read.
    var luaSig: String {
        pages.filter { $0.s("key") == "lua" && !($0.s("name") ?? "").isEmpty }
            .map { "\($0.s("name") ?? "")=\($0.b("effectOn") ?? true)" }.joined(separator: ",")
    }
    func index(key: String, name: String) -> Int? { pages.first { $0.s("key") == key && $0.s("name") == name }?.i("i") }
}

struct Effects {
    var bytes: [String: Int] = [:]      // shown name -> size of the uploaded script
    var stem: [String: String] = [:]    // shown name -> its file's stem on this device
    var names: [String] = []            // effects[], in order: the index for walk
    var walk: [String: Bool] = [:]
    var fields: [String: String] {
        var f: [String: String] = [:]
        for (n, b) in bytes { f["s." + n] = "\(b)"; f["w." + n] = walk[n] == false ? "0" : "1" }
        return f
    }
}

struct Firmware {
    var version = "", build = "", bytes = 0, otaFree = 0, state = "", rolledBackFrom = "", mac = "", name = "", model = ""
    init() {}
    init(_ i: J) {
        version = i.s("version") ?? ""; build = i.s("build") ?? ""; bytes = i.i("firmwareBytes") ?? 0
        otaFree = i.i("otaFreeBytes") ?? 0; mac = normMac(i.s("mac")); name = i.s("deviceName") ?? ""; model = i.s("model") ?? ""
        let ota = i.o("ota") ?? [:]
        state = ota.s("state") ?? ""; rolledBackFrom = ota.s("rolledBackFrom") ?? ""
    }
    /// An image is known by version, build time and size: the firmware gives out no hash of it.
    var id: String { version.isEmpty ? "" : "\(version) (\(build), \(bytes) B)" }
}

// MARK: - the settings as one flat dictionary (tools/twin/sync.py settings_of, both ways)

enum Settings {
    // Not compared: identity, secrets, state, what the screen mirror carries, the zone (compared as
    // "tz"), the owner's override (climateHa).
    static let exportSkip: Set<String> = ["deviceName", "weatherApiKey", "metricNames", "clockStyle", "timezoneString", "gmtOffset",
                                          "daylightSaving", "climateHa"]
    // The form's own: identity and the network (a useStaticIP change restarts, web.cpp:2251-2263), the
    // key, what the screen mirror carries, the zone's region, a card marker, and the export's keys
    // under other names (rowMode = displayRowMode, rpmKFormat = useRpmKFormat, netMBFormat =
    // useNetworkMBFormat, weatherFahrenheit = weatherUseFahrenheit; web.cpp:1599-1607, 1713).
    static let formIdentity: Set<String> = ["deviceName", "useStaticIP", "staticIP", "gateway", "subnet", "dns1", "dns2"]
    static let formSkip: Set<String> = formIdentity.union(["weatherApiKey", "displayBrightness", "clockStyle", "timezoneRegion", "irCard",
                                                           "rowMode", "rpmKFormat", "netMBFormat", "weatherFahrenheit"])
    // The owner's overrides keep these pages out of the twin's walk; they are not mirrored either way.
    static let pageSkip: Set<String> = ["trains", "flights", "cards", "clock"]
    static let metricFields: [(String, String)] = [("metricLabels", "label_"), ("metricOrder", "order_"), ("metricCompanions", "companion_"),
        ("metricPositions", "position_"), ("metricBarPositions", "barPosition_"), ("metricBarMin", "barMin_"), ("metricBarMax", "barMax_"),
        ("metricBarWidths", "barWidth_"), ("metricBarOffsets", "barOffset_")]   // web.cpp:2047-2120; config.h MAX_METRICS 20
    static let carouselKeys = ["enabled", "idleS", "slotS", "allStyles"]
    static let knobKeys = ["reverse", "lockoutMs", "debounceMs", "detent"]
    static let railKeys = ["rows", "switch_s", "level", "stale_s", "due_min", "clock_seconds", "row_color", "head_color", "due_color"]
    static let budgetKeys = ["floor_min", "day_cap", "month_cap"]
    static let noAsk = (icao: "ZZZZ", name: "NO REQUESTS")      // sync.py NO_ASK

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
            s["flightboard.custom"] = fb.a("airports").filter { $0.s("kind") == "custom" && $0.s("code") != noAsk.icao }
                .map { [$0.s("code") ?? "", $0.s("iata") ?? "", $0.s("name") ?? "", $0.s("tz") ?? ""] }
                .sorted { canon($0) < canon($1) }
            let direct = fb.o("direct") ?? [:]
            if direct.b("built") != false, let bud = direct.o("budget") { s["flightboard.budget"] = pick(bud, budgetKeys) }
            s["flightboard.tracked"] = fb.a("tracked").compactMap { $0.s("ident") }.sorted()
        }
        if let cfg = d["/api/market"]?.o("config") { for (k, v) in cfg { s["market." + k] = v } }
        if let md = d["/api/media"] { s["media.selected"] = md.s("selected") ?? "" }
        for bt in d["/api/ir"]?.a("buttons") ?? [] {
            if let n = bt.i("n") { s["ir.\(n).fn"] = [bt.s("fn") ?? "none", bt["page"] ?? NSNull()] as [Any] }
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
}

// MARK: - panels on the network (mDNS)

/// The firmware announces _http._tcp with TXT version, model and mac (network.cpp:263-266).
final class PanelFinder: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    struct Found: Equatable { let service: String, name: String, address: String, mac: String, version: String }
    private let browser = NetServiceBrowser()
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
    struct Summary { var panel = "", twin = "", panelFirmware = "", twinFirmware = "", settings: [String] = [],
                     onlyPanel: [String] = [], onlyTwin: [String] = [], differ: [String] = [], panelScreen = "", twinScreen = "" }
    struct Offer { let id: String, version: String, build: String, panelVersion: String, panelBuild: String, source: Msg }
    struct Status { var phase = M("off", "выключена"); var peer = ""; var last: (Date, Msg)?; var problem: (Date, Msg)?; var sticky = false }

    // Thresholds of our choosing, not measured: a change on the twin bigger than this waits for the person.
    static let MASS_SETTINGS = 12, MASS_DELETES = 2
    static let fastEvery: TimeInterval = 3, effectsEvery: TimeInterval = 60

    let dataDir: URL
    var logFile: URL { dataDir.appendingPathComponent("sync.log") }
    var stateFile: URL { dataDir.appendingPathComponent("sync-state.json") }

    // Shared with the main thread, under the lock.
    private let lock = NSLock()
    private var _enabled = false, _twinAddress: String?, _twinMac = "", _found: [PanelFinder.Found] = []
    private var _lang = "en", _syncNow = false, _reset = false
    private var _direction: Direction?, _firmwareAnswer: (String, Bool)?
    private var _status = Status()
    /// Called on the main thread when the status changed.
    var onStatus: (() -> Void)?
    /// The main thread asks the person; the answers come back through answerDirection / answerFirmware.
    var askDirection: ((Summary) -> Void)?
    var askFirmware: ((Offer) -> Void)?

    private func locked<T>(_ f: () -> T) -> T { lock.lock(); defer { lock.unlock() }; return f() }
    var enabled: Bool { locked { _enabled } }
    var status: Status { locked { _status } }
    func setEnabled(_ on: Bool) { locked { _enabled = on; if !on { _reset = true } } }
    func setTwin(address: String?, mac: String) { locked { _twinAddress = address; _twinMac = normMac(mac) } }
    func setFound(_ f: [PanelFinder.Found]) { locked { _found = f } }
    func setLanguage(_ l: String) { locked { _lang = l } }
    func syncNow() { locked { _syncNow = true } }
    /// The panel's address or the repository changed: find the panel again.
    func reconnect() { locked { _reset = true } }
    func answerDirection(_ d: Direction) { locked { _direction = d } }
    func answerFirmware(id: String, go: Bool) { locked { _firmwareAnswer = (id, go) } }

    // The worker's own.
    private var thread: Thread?
    private var dev: [Side: Device] = [:]
    private var pairKey = ""
    private var aligned = false, askedDirection = false, resumed = false
    private var base: [Side: [String: [String: String]]] = [:]    // side -> "screen"|"settings"|"effects" -> field -> value
    private var firmwareBase: [Side: String] = [:]
    private var screen: [Side: Screen] = [:]
    private var effects: [Side: Effects] = [:]
    private var docs: [Side: [String: J]] = [:]
    private var fw: [Side: Firmware] = [:]
    private var caps: [Side: (id: String, lua: Bool, image: Bool)] = [:]
    private var pendingFx: [Side: [String: Int]] = [:]              // effects that could not be carried yet
    private var noted = Set<String>()
    private var nextFast = Date.distantPast, nextSlow = Date.distantPast, nextEffects = Date.distantPast
    private var fxDue = false, slowRounds = 0, fwWatch: Date?
    private var offer: Offer?, offerFrom: Side?, declined = Set<String>()
    private var gallery: (at: Date, repo: String, list: [J])?
    private var stateDirty = false, lastSaved: Data?
    private var down = Set<Side>()
    private var failures: [String: Int] = [:]

    init(dataDir: URL) { self.dataDir = dataDir }

    func start() {
        guard thread == nil else { return }
        let t = Thread { [weak self] in
            while let self {
                self.step()
                Thread.sleep(forTimeInterval: 0.5)
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
        if let h = try? FileHandle(forWritingTo: logFile) { h.seekToEndOfFile(); h.write(Data(line.utf8)); try? h.close() }
        else { try? line.write(to: logFile, atomically: false, encoding: .utf8) }
        publish { if problem { $0.problem = (Date(), m); $0.sticky = sticky } else { $0.last = (Date(), m); $0.problem = nil } }
    }
    /// A standing condition: logged once (until sync starts again).
    private func noteOnce(_ key: String, _ m: Msg) { if noted.insert(key).inserted { log("note", "-", m, problem: true, sticky: true) } }

    // MARK: the loop

    private func step() {
        let (on, reset) = locked { () -> (Bool, Bool) in let r = _reset; _reset = false; return (_enabled, r) }
        if reset || !on { if !dev.isEmpty || aligned || askedDirection { forget() } }
        guard on else { phase(M("off", "выключена")); return }
        do {
            try connect()
            guard aligned else { try align(); return }
            if let (id, go) = locked({ () -> (String, Bool)? in let a = _firmwareAnswer; _firmwareAnswer = nil; return a }) { try answered(id: id, go: go) }
            let now = Date()
            let forced = locked { () -> Bool in let f = _syncNow; _syncNow = false; return f }
            if forced { retryPending() }
            if now >= nextFast || forced { try fastRound(); nextFast = Date().addingTimeInterval(SyncEngine.fastEvery) }
            if fxDue || forced || now >= nextEffects {
                fxDue = false
                try effectsRound(); nextEffects = Date().addingTimeInterval(SyncEngine.effectsEvery)
            }
            if forced || now >= nextSlow || (fwWatch.map { now >= $0 } ?? false) {
                fwWatch = nil
                try slowRound(market: forced || slowRounds % 5 == 0); slowRounds += 1
                nextSlow = Date().addingTimeInterval(settingsEvery)
            }
            saveState()
            phase(M("on", "включена"))
            if !down.isEmpty {
                log("note", "back", M("both devices answer again", "оба устройства снова отвечают")); down = []
            }
            if locked({ _status.problem != nil && !_status.sticky }) { publish { $0.problem = nil } }
        } catch let w as SyncWait {
            phase(w.msg)
            Thread.sleep(forTimeInterval: 2)
        } catch let d as SyncDown {
            if down.insert(d.side).inserted { log("error", "down", d.msg, problem: true) }
            phase(d.msg)
            nextFast = Date().addingTimeInterval(5)
            Thread.sleep(forTimeInterval: 3)
        } catch let e as SyncError {
            log("error", "-", e.msg, problem: true)
            nextFast = Date().addingTimeInterval(5)
            Thread.sleep(forTimeInterval: 3)
        } catch {
            log("error", "-", M("\(error)", "\(error)"), problem: true)
            Thread.sleep(forTimeInterval: 3)
        }
    }

    private var settingsEvery: TimeInterval {
        let s = UserDefaults.standard.integer(forKey: "syncSettingsEveryS")
        return TimeInterval(s <= 0 ? 60 : max(15, s))
    }
    private var repo: String { GitHub.repo }

    private func forget() {
        saveState()
        dev = [:]; pairKey = ""; aligned = false; askedDirection = false; resumed = false
        base = [:]; firmwareBase = [:]; screen = [:]; effects = [:]; docs = [:]; fw = [:]; caps = [:]; pendingFx = [:]; noted = []
        offer = nil; offerFrom = nil; fwWatch = nil; slowRounds = 0; down = []; failures = [:]
        nextFast = .distantPast; nextSlow = .distantPast; nextEffects = .distantPast
        locked { _direction = nil; _firmwareAnswer = nil; _status.peer = ""; _status.problem = nil }
    }

    // MARK: who is who

    private var testing: Bool { UserDefaults.standard.bool(forKey: "syncPanelMayBeTwinForTesting") }

    private func connect() throws {
        let (tAddr, tMac, found) = locked { (_twinAddress, _twinMac, _found) }
        guard let tAddr, !tMac.isEmpty else { throw SyncWait(M("waiting for the twin", "жду двойника")) }
        if dev[.twin]?.address != tAddr {
            let d = Device(.twin, tAddr)
            guard let info = try? d.get("/api/info") else { throw SyncWait(M("waiting for the twin's firmware", "жду прошивку двойника")) }
            let f = Firmware(info)
            guard f.name.hasPrefix("TWIN-") else { throw SyncWait(M("waiting for the twin to be named TWIN-…", "жду, пока двойник получит имя TWIN-…")) }
            guard f.mac == tMac else { throw SyncError(M("\(tAddr) answers with MAC \(f.mac), not this twin's \(tMac)", "\(tAddr) отвечает с MAC \(f.mac), а у этого двойника \(tMac)")) }
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
            dev[.panel] = d; fw[.panel] = f
            publish { $0.peer = "\(f.name) (\(pAddr))" }
        }
        let key = "\(fw[.panel]!.mac)|\(fw[.twin]!.mac)"
        if key != pairKey {
            base = [:]; firmwareBase = [:]; pendingFx = [:]; noted = []; resumed = false
            pairKey = key; aligned = false; askedDirection = false; loadState()
        }
    }

    /// Why the device at ADDRESS must not be synced with as the panel, or nil.
    private func refusal(_ f: Firmware, address: String, twinMac: String, twinAddress: String) -> Msg? {
        if f.model != "AnimatedPixelClock" { return M("\(address) is not an AnimatedPixelClock panel", "\(address) — не панель AnimatedPixelClock") }
        if f.mac == twinMac || address == twinAddress {
            return M("\(address) is this twin itself: a twin is not synced with itself", "\(address) — это сам двойник: синхронизировать двойника с самим собой нельзя")
        }
        if twinLike(name: f.name, mac: f.mac) && !testing {
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
        let want = normMac(d.string(forKey: "panelMac"))
        let panels = found.filter { $0.mac != twinMac && (testing || !twinLike(name: $0.name, mac: $0.mac)) }
        if !want.isEmpty {
            if let p = panels.first(where: { $0.mac == want }) { return p.address }
            throw SyncWait(M("looking for the panel \(want) on the network", "ищу в сети панель \(want)"))
        }
        if panels.count == 1 { return panels[0].address }
        if panels.isEmpty { throw SyncWait(M("looking for the panel on the network (mDNS)", "ищу панель в сети (mDNS)")) }
        throw SyncWait(M("\(panels.count) panels on the network: choose one in Sync → Which panel…",
                         "в сети \(panels.count) панелей: выберите в меню «Синхронизация → Какая панель…»"))
    }

    private func device(_ s: Side) -> Device { dev[s]! }

    // MARK: reading

    private func readScreen(_ s: Side) throws -> Screen {
        let d = device(s)
        guard let pn = try d.get("/api/panel"), let st = try d.get("/api/status") else {
            throw SyncError(M("\(s.word.en) has no /api/panel: its firmware is too old for sync", "\(s.at) нет /api/panel: прошивка слишком старая для синхронизации"))
        }
        var x = Screen()
        let now = pn.o("now") ?? [:], car = pn.o("carousel") ?? [:]
        x.key = now.s("key") ?? ""; x.name = now.s("name") ?? ""; x.page = now.i("page") ?? 0; x.style = now.i("style") ?? 0
        x.running = car.b("running") ?? false; x.allStyles = car.b("allStyles") ?? false; x.pageS = car.i("pageS") ?? 0
        x.pages = pn.a("pages"); x.styles = Set(pn.a("styles").compactMap { $0.i("id") })
        x.bright = st.i("brightness") ?? 0; x.off = st.b("forcedOff") ?? false; x.uptime = st.i("uptime") ?? 0
        return x
    }

    private func readEffects(_ s: Side) throws -> Effects {
        var e = Effects()
        guard let lua = try device(s).get("/api/lua") else { return e }       // a build without Lua
        e.names = lua["effects"] as? [String] ?? []
        let walk = (lua["inWalk"] as? [Any] ?? []).map { ($0 as? NSNumber)?.boolValue ?? true }
        for (i, n) in e.names.enumerated() { e.walk[n] = i < walk.count ? walk[i] : true }
        for sc in (lua.o("uploaded")?["scripts"] as? [J]) ?? [] {
            guard let stem = sc.s("name"), let b = sc.i("bytes") else { continue }
            e.bytes[shownName(stem)] = b; e.stem[shownName(stem)] = stem
        }
        return e
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

    // MARK: first alignment

    private func readAll() throws {
        for s in [Side.panel, .twin] {
            screen[s] = try readScreen(s)
            try readDocs(s, SyncEngine.settingsRoutes + ["/api/market"])
            effects[s] = try readEffects(s)
        }
    }

    private func align() throws {
        if resumed || !(base[.panel]?["settings"] ?? [:]).isEmpty {
            // The pair was synced before (sync-state.json): what changed meanwhile goes as any change does.
            // The screen follows the panel: the twin starts on its clock.
            try readAll()
            try mirrorScreen(from: .panel, fields: ["page", "style", "bright", "off"])
            rebaseScreen(.panel); rebaseScreen(.twin)
            for s in [Side.panel, .twin] { base[s, default: [:]]["effects"] = base[s]?["effects"] ?? effects[s]!.fields }
            aligned = true; resumed = true
            retryPending()
            log("note", "resume", M("resumed with \(fw[.panel]!.name): changes made while sync was off are mirrored now",
                                    "продолжаю с \(fw[.panel]!.name): изменения, сделанные без синхронизации, переношу сейчас"))
            return
        }
        var dir = locked { () -> Direction? in let d = _direction; _direction = nil; return d }
        if dir == nil, twinLike(name: fw[.panel]!.name, mac: fw[.panel]!.mac), testing {
            switch UserDefaults.standard.string(forKey: "syncAlignForTesting") {
            case "panel": dir = .fromPanel
            case "twin": dir = .fromTwin
            default: break
            }
        }
        guard let dir else {
            if !askedDirection {
                try readAll()
                askedDirection = true
                let sum = summary()
                DispatchQueue.main.async { self.askDirection?(sum) }
            }
            throw SyncWait(M("waiting for your answer: which way first", "жду ответа: в какую сторону сначала"))
        }
        let from: Side = dir == .fromPanel ? .panel : .twin
        phase(M("first alignment: \(from.arrow)", "первое выравнивание: \(from.arrow)"))
        log(from.arrow, "align", M("first alignment, \(from.word.en) as it is", "первое выравнивание: берётся \(from.word.ru) как есть"))
        try readAll()
        let to = from.other
        if fw[from]!.id != fw[to]!.id { try offerFirmware(from: from) }
        let keys = settingsDiff(from: from)
        if !keys.isEmpty { try applySettings(from: from, keys: keys) }
        if to == .twin { try enforceOverrides() }
        try convergeEffects(from: from)
        try readAll()
        try mirrorScreen(from: from, fields: ["page", "style", "bright", "off"])
        try readAll()
        for s in [Side.panel, .twin] {
            rebaseScreen(s)
            base[s, default: [:]]["settings"] = digests(flat(s))
            base[s, default: [:]]["effects"] = effects[s]!.fields
            if firmwareBase[s] == nil || fw[s]!.state == "valid" { firmwareBase[s] = fw[s]!.id }
        }
        aligned = true; stateDirty = true
        if fw[.panel]!.mac != normMac(UserDefaults.standard.string(forKey: "panelMac")),
           (UserDefaults.standard.string(forKey: "panelAddress") ?? "").isEmpty {
            UserDefaults.standard.set(fw[.panel]!.mac, forKey: "panelMac")     // from now on this panel, by its MAC
        }
        log(from.arrow, "align", M("aligned: settings \(keys.count)", "выровнено: настроек \(keys.count)"))
    }

    private func settingsDiff(from s: Side) -> [String] {
        let a = flat(s), b = flat(s.other)
        return a.keys.filter { b[$0] != nil && canon(a[$0]) != canon(b[$0]) }.sorted()
    }

    private func summary() -> Summary {
        var m = Summary()
        m.panel = "\(fw[.panel]!.name) (\(device(.panel).address))"; m.twin = "\(fw[.twin]!.name) (\(device(.twin).address))"
        m.panelFirmware = fw[.panel]!.id; m.twinFirmware = fw[.twin]!.id
        m.settings = settingsDiff(from: .panel).map { String($0.split(separator: ".", maxSplits: 1).last ?? "") }
        let p = effects[.panel]!, t = effects[.twin]!
        m.onlyPanel = p.bytes.keys.filter { t.bytes[$0] == nil }.sorted()
        m.onlyTwin = t.bytes.keys.filter { p.bytes[$0] == nil }.sorted()
        m.differ = p.bytes.keys.filter { t.bytes[$0] != nil && t.bytes[$0] != p.bytes[$0] }.sorted()
        m.panelScreen = screen[.panel]!.name; m.twinScreen = screen[.twin]!.name
        return m
    }

    // MARK: the screen

    private func rebaseScreen(_ s: Side) { if let x = screen[s] { base[s, default: [:]]["screen"] = x.fields } }

    private func fastRound() throws {
        var fresh: [Side: Screen] = [:]
        for s in [Side.panel, .twin] { fresh[s] = try readScreen(s) }
        for s in [Side.panel, .twin] {
            let x = fresh[s]!, prev = screen[s]
            screen[s] = x
            if let prev {
                if prev.luaSig != x.luaSig { fxDue = true }
                if x.uptime < prev.uptime { rebooted(s, x) }
            }
        }
        let cur = screen                                    // after a reboot's own handling
        if base[.panel]?["screen"] == nil || base[.twin]?["screen"] == nil { rebaseScreen(.panel); rebaseScreen(.twin); return }
        var changed: [Side: Set<String>] = [:]
        for s in [Side.panel, .twin] {
            let b = base[s]!["screen"]!, f = cur[s]!.fields
            var c = Set(f.keys.filter { f[$0] != b[$0] })
            // A page or style the carousel put there is its own walk, not a person's choice.
            if cur[s]!.running { c.remove("page"); if cur[s]!.allStyles { c.remove("style") } }
            changed[s] = c
        }
        // Both changed one field: the page to the one changed last (smaller pageS), the rest to the panel.
        for f in changed[.panel]!.intersection(changed[.twin]!) where cur[.panel]!.fields[f] != cur[.twin]!.fields[f] {
            if f == "page" && cur[.twin]!.pageS < cur[.panel]!.pageS { changed[.panel]!.remove(f) } else { changed[.twin]!.remove(f) }
        }
        for s in [Side.panel, .twin] {
            let c = changed[s]!
            if !c.isEmpty {
                do { try mirrorScreen(from: s, fields: c); forgive("screen", s, Array(c)) }
                catch { if tryAgain("screen", s, Array(c)) { throw error } }      // the base stays: seen again next round
            }
            rebaseScreen(s)
        }
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

    private func rebooted(_ s: Side, _ x: Screen) {
        log("note", "reboot", M("\(s.word.en) restarted (uptime \(x.uptime) s): its reset screen is not mirrored",
                                s == .panel ? "панель перезагрузилась (uptime \(x.uptime) с): её сброшенный экран не переношу"
                                            : "двойник перезагрузился (uptime \(x.uptime) с): он повторяет экран панели"))
        screen[s] = x; rebaseScreen(s)
        fwWatch = Date().addingTimeInterval(5)
        caps[s] = nil
        if s == .twin, aligned { try? mirrorScreen(from: .panel, fields: ["page", "style", "bright", "off"]) }  // the twin follows the panel
    }

    /// Makes the other side's screen show FIELDS of side S's, then reads it again as its base.
    private func mirrorScreen(from s: Side, fields: Set<String>) throws {
        let o = s.other
        guard let src = screen[s] else { return }
        var dst = try readScreen(o)
        var body: J = [:], did: [String] = []
        if fields.contains("page"), src.shown != dst.shown {
            if src.key == "cards" {
            } else if o == .twin && (src.key == "trains" || src.key == "flights") {
                noteOnce("page-\(src.key)", M("the \(src.name) page is not shown on the twin (the owner's override)", "страницу \(src.name) на двойнике не показываю (переопределение владельца)"))
            } else if let i = dst.index(key: src.key, name: src.name) {
                body["show"] = ["page": i]; did.append(M("page \(src.name)", "страница \(src.name)").text(lang))
            } else {
                noteOnce("nopage-\(o)-\(src.shown)", M("\(o.word.en) has no page \(src.name)", "\(o.on) нет страницы \(src.name)"))
            }
        }
        if fields.contains("style"), src.style != dst.style {
            if dst.styles.contains(src.style) {
                body["style"] = src.style; did.append(M("clock style \(src.style)", "стиль часов \(src.style)").text(lang))
                // style moves to the clock page (panelShowStyle, main.cpp:906-909): keep the page meant.
                if body["show"] == nil, src.key != "clock", dst.key != "cards" { body["show"] = ["page": dst.page] }
            } else {
                noteOnce("style-\(o)-\(src.style)", M("\(o.word.en) has no clock style \(src.style)", "\(o.at) нет стиля часов \(src.style)"))
            }
        }
        if !body.isEmpty { try device(o).post("/api/panel", body) }
        var offNow = dst.off
        if fields.contains("bright"), src.bright != dst.bright {
            try device(o).action("/api/display/brightness", [("value", "\(src.bright)")])
            offNow = src.bright == 0                                   // display.cpp:193
            did.append(M("brightness \(src.bright)%", "яркость \(src.bright)%").text(lang))
        }
        if fields.contains("off") || fields.contains("bright"), src.off != offNow {
            try device(o).action(src.off ? "/api/display/off" : "/api/display/on")
            did.append(src.off ? M("screen off", "экран выключен").text(lang) : M("screen on", "экран включён").text(lang))
        }
        guard !did.isEmpty else { return }
        dst = try readScreen(o); screen[o] = dst; rebaseScreen(o)
        log(s.arrow, "screen", M(did.joined(separator: ", "), did.joined(separator: ", ")))
    }

    // MARK: effects

    private func effectsRound() throws {
        var cur: [Side: Effects] = [:]
        for s in [Side.panel, .twin] { cur[s] = try readEffects(s); effects[s] = cur[s] }
        guard let bp = base[.panel]?["effects"], let bt = base[.twin]?["effects"] else {
            for s in [Side.panel, .twin] { base[s, default: [:]]["effects"] = cur[s]!.fields }
            return
        }
        var changed: [Side: [String]] = [:]
        for (s, b) in [(Side.panel, bp), (.twin, bt)] {
            let f = cur[s]!.fields
            // A walk switch of an effect that just came or went is part of that change.
            changed[s] = Set(f.keys).union(b.keys).filter { f[$0] != b[$0] }
                .filter { !$0.hasPrefix("w.") || (f[$0] != nil && b[$0] != nil) }.sorted()
        }
        for k in Set(changed[.panel]!).intersection(changed[.twin]!) where cur[.panel]!.fields[k] != cur[.twin]!.fields[k] {
            changed[.twin]!.removeAll { $0 == k }                     // no clock for these: the panel's stands
        }
        let deletes = changed[.twin]!.filter { $0.hasPrefix("s.") && cur[.twin]!.fields[$0] == nil }.count
        if deletes > SyncEngine.MASS_DELETES {
            massChange(M("\(deletes) effects removed on the twin at once", "на двойнике сразу удалено эффектов: \(deletes)")); return
        }
        // Both bases first: applying to a side reads it again and makes that its base.
        for s in [Side.panel, .twin] { base[s, default: [:]]["effects"] = cur[s]!.fields }
        for s in [Side.panel, .twin] where !changed[s]!.isEmpty {
            let keys = changed[s]!, old = s == .panel ? bp : bt
            do { try applyEffects(from: s, keys: keys); forgive("effects", s, keys) }
            catch {
                // Seen as changed again next time (three times at most): the old base comes back.
                if tryAgain("effects", s, keys) { for k in keys { base[s]!["effects"]![k] = old[k] } }
                throw error
            }
        }
    }

    private func convergeEffects(from s: Side) throws {
        let a = effects[s]!.fields, b = effects[s.other]!.fields
        let keys = Set(a.keys).union(b.keys).filter { a[$0] != b[$0] }.sorted()
        if !keys.isEmpty { try applyEffects(from: s, keys: keys) }
    }

    /// Carries effect changes of side S to the other side: removals, then new and replaced scripts, then walk switches.
    private func applyEffects(from s: Side, keys: [String]) throws {
        let o = s.other, src = effects[s]!
        var dst = try readEffects(o)
        var wrote = false
        for k in keys where k.hasPrefix("s.") && src.bytes[String(k.dropFirst(2))] == nil {
            let name = String(k.dropFirst(2))
            pendingFx[s]?[name] = nil
            guard let stem = dst.stem[name] else { continue }
            try device(o).post("/api/lua", ["delete": stem])                  // puts the clock on (web_panel.cpp:1004-1005)
            wrote = true
            log(s.arrow, "effect", M("removed \(name)", "удалён эффект \(name)"))
        }
        if wrote { dst = try readEffects(o) }                                  // a removal renumbers the list
        for k in keys where k.hasPrefix("s.") {
            let name = String(k.dropFirst(2))
            guard let bytes = src.bytes[name], let stem = src.stem[name], dst.bytes[name] != bytes else { continue }
            guard let script = try script(from: s, name: name, stem: stem, bytes: bytes) else {
                pendingFx[s, default: [:]][name] = bytes
                noteOnce("fx-\(s)-\(name)-\(bytes)", M("\(name) is not carried to \(o.word.en): the firmware of \(s.word.en) has no /api/lua/source, and the gallery of \(repo) has no such file",
                                                        "\(name) не перенести на \(o.to): в прошивке \(s.of) нет /api/lua/source, а в галерее \(repo) такого файла нет"))
                continue
            }
            let target = dst.stem[name] ?? stem                               // the same effect keeps the target's file name
            try uploadEffect(o, stem: target, data: script)
            pendingFx[s]?[name] = nil
            wrote = true
            log(s.arrow, "effect", M("\(dst.bytes[name] == nil ? "new" : "replaced") \(name) (\(bytes) B)",
                                     "\(dst.bytes[name] == nil ? "новый" : "заменён") эффект \(name) (\(bytes) Б)"))
            dst = try readEffects(o)
            if src.walk[name] == false, dst.walk[name] != false, let i = dst.names.firstIndex(of: name) {
                try device(o).post("/api/lua", ["walk": ["i": i, "on": false, "name": name] as J])
                dst = try readEffects(o)
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
        effects[o] = try readEffects(o)
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
    /// the same name and size; nil when neither has it.
    private func script(from s: Side, name: String, stem: String, bytes: Int) throws -> Data? {
        if capabilities(s).lua {
            let d = try device(s).luaSource(stem)
            guard d.count == bytes else { throw SyncError(M("\(name): \(d.count) bytes read, the list says \(bytes)", "\(name): прочитано \(d.count) байт, в списке \(bytes)")) }
            return d
        }
        guard let g = galleryList(),
              let e = g.first(where: { $0.i("bytes") == bytes && (($0.s("stem") ?? "").lowercased() == stem.lowercased() || $0.s("name") == name) }),
              let file = e.s("file"), file.range(of: #"^[A-Za-z0-9_.-]+\.lua$"#, options: .regularExpression) != nil else { return nil }
        let d = try fetch("https://raw.githubusercontent.com/\(repo)/main/gallery/\(file)", limit: 1 << 20)
        return d.count == bytes ? d : nil
    }

    private func galleryList() -> [J]? {
        if let g = gallery, g.repo == repo, Date().timeIntervalSince(g.at) < 600 { return g.list }
        guard let d = try? fetch("https://raw.githubusercontent.com/\(repo)/main/gallery/index.json", limit: 4 << 20),
              let o = try? JSONSerialization.jsonObject(with: d) as? J else { return nil }
        gallery = (Date(), repo, o.a("effects"))
        return gallery!.list
    }

    private func capabilities(_ s: Side) -> (id: String, lua: Bool, image: Bool) {
        let id = fw[s]?.id ?? ""
        if let c = caps[s], c.id == id { return c }
        let c = (id: id, lua: device(s).hasLuaSource(), image: device(s).hasImageRoute())
        caps[s] = c
        return c
    }

    /// Effects that could not be carried: tried again when the source side's firmware or the repository changed.
    private func retryPending() {
        for s in [Side.panel, .twin] {
            guard let p = pendingFx[s], !p.isEmpty else { continue }
            noted = noted.filter { !$0.hasPrefix("fx-\(s)-") }
            if let e = effects[s] { try? applyEffects(from: s, keys: p.keys.filter { e.bytes[$0] == p[$0] }.map { "s." + $0 }) }
        }
    }

    // MARK: settings and firmware

    private func slowRound(market: Bool) throws {
        for s in [Side.panel, .twin] { try readDocs(s, SyncEngine.settingsRoutes + (market ? ["/api/market"] : [])) }
        // Who is who, again: an address can change hands.
        if fw[.panel]!.mac != pairKey.split(separator: "|").first.map(String.init) || !fw[.twin]!.name.hasPrefix("TWIN-") {
            reconnect(); throw SyncWait(M("the devices changed: connecting again", "устройства сменились: подключаюсь заново"))
        }
        try firmwareRound()
        let cur: [Side: J] = [.panel: flat(.panel), .twin: flat(.twin)]
        let dig = cur.mapValues(digests)
        guard let bp = base[.panel]?["settings"], let bt = base[.twin]?["settings"], !bp.isEmpty, !bt.isEmpty else {
            for s in [Side.panel, .twin] { base[s, default: [:]]["settings"] = dig[s]! }
            try enforceOverrides(); return
        }
        var changed: [Side: [String]] = [:]
        for (s, b) in [(Side.panel, bp), (.twin, bt)] {
            // A key new on this side (a newer firmware, a module that appeared) is not a change of anyone's.
            changed[s] = dig[s]!.keys.filter { b[$0] != nil && b[$0] != dig[s]![$0] }.sorted()
        }
        for k in Set(changed[.panel]!).intersection(changed[.twin]!) { changed[.twin]!.removeAll { $0 == k } }
        if changed[.twin]!.count > SyncEngine.MASS_SETTINGS {
            massChange(M("\(changed[.twin]!.count) settings changed on the twin at once", "на двойнике сразу изменилось настроек: \(changed[.twin]!.count)")); return
        }
        for s in [Side.panel, .twin] { base[s, default: [:]]["settings"] = dig[s]! }
        for s in [Side.panel, .twin] {
            let o = s.other
            var keys: [String] = []
            for k in changed[s]! {
                guard cur[o]![k] != nil else {
                    noteOnce("key-\(o)-\(k)", M("\(k): \(o.word.en)'s firmware has no such setting", "\(k): в прошивке \(o.of) такой настройки нет")); continue
                }
                if canon(cur[o]![k]) != canon(cur[s]![k]) { keys.append(k) }
            }
            guard !keys.isEmpty else { continue }
            let old = s == .panel ? bp : bt
            do { try applySettings(from: s, keys: keys); forgive("settings", s, keys) }
            catch {
                if tryAgain("settings", s, keys) { for k in keys { base[s]!["settings"]![k] = old[k] } }
                throw error
            }
        }
        try enforceOverrides()
    }

    private func massChange(_ m: Msg) {
        log("note", "hold", M("\(m.en): not sent to the panel - which way?", "\(m.ru): на панель не отправляю — в какую сторону?"), problem: true)
        base = [:]; aligned = false; askedDirection = false; resumed = false
    }

    /// Writes the settings KEYS of side S to the other side, then reads what it wrote again as the other side's base.
    private func applySettings(from s: Side, keys: [String]) throws {
        let o = s.other, dv = device(o)
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
            // The form is the target's own, read just now, with the changed fields replaced.
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
            if fn == "page", let page = v.last as? NSNumber { p.append(("page", page.stringValue)) }
            try dv.action("/api/ir/fn", p); done.append(k)
        }
        if keys.contains("logOn") { try dv.action("/api/log", [("on", (src["logOn"] as? NSNumber)?.boolValue == true ? "1" : "0")]); done.append("logOn") }
        guard !done.isEmpty else { return }
        // Read back what was written: the target's new values become its base, so they are not sent back.
        let routes = Array(Set(keys.flatMap(Settings.routes))).filter { Device.reads.contains($0) }.sorted()
        try readDocs(o, routes)
        if o == .twin { try enforceOverrides() }
        base[o, default: [:]]["settings"] = digests(flat(o))
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
    /// the AeroAPI budget, tracked flights (sync.py plan_flightboard). The selection is the owner's override on
    /// the twin and is never mirrored.
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
        return done
    }

    /// The owner's overrides on the twin (sync.py OVERRIDES), put back whenever they drift. None of them is
    /// ever compared or sent to the panel.
    private func enforceOverrides() throws {
        let tw = device(.twin), d = docs[.twin] ?? [:]
        var did: [String] = []
        if d["/api/export"]?.b("climateHa") == true {
            try tw.post("/api/import", ["climateHa": false]); did.append("climateHa false")    // climate.cpp:127-131, 165-182
        }
        for p in d["/api/panel"]?.a("pages") ?? [] where ["trains", "flights"].contains(p.s("key") ?? "") && p.b("on") == true {
            try tw.post("/api/panel", ["enable": ["key": p.s("key")!, "on": false] as J]); did.append("\(p.s("key")!) off")
        }
        if let fb = d["/api/flightboard"] {
            let apts = fb.a("airports")
            let sel = apts.first { $0.i("id") == fb.i("airport") }
            if sel?.s("code") != Settings.noAsk.icao {
                let dir = docs[.panel]?["/api/flightboard"]?.s("dir") ?? fb.s("dir") ?? "alt"
                if let mine = apts.first(where: { $0.s("kind") == "custom" && $0.s("code") == Settings.noAsk.icao }) {
                    try tw.post("/api/flightboard", ["airport": mine["id"] ?? 0, "dir": dir])
                } else if apts.filter({ $0.s("kind") == "custom" }).count < (fb.o("limits")?.i("custom") ?? 6) {
                    let tz = (docs[.panel]?["/api/flightboard"]?.a("airports").first { $0.i("id") == docs[.panel]?["/api/flightboard"]?.i("airport") })?.s("tz") ?? "Europe/Paris"
                    try tw.post("/api/flightboard", ["add": ["icao": Settings.noAsk.icao, "iata": "", "name": Settings.noAsk.name, "tz": tz, "select": true] as J, "dir": dir])
                } else {
                    noteOnce("noask-full", M("the twin's six custom airports are all used: ZZZZ cannot be selected", "у двойника заняты все шесть своих аэропортов: ZZZZ не выбрать"))
                }
                did.append("flight board ZZZZ")
            }
        }
        guard !did.isEmpty else { return }
        try readDocs(.twin, ["/api/export", "/api/portal", "/api/panel", "/api/flightboard"])
        if base[.twin]?["settings"] != nil { base[.twin]!["settings"] = digests(flat(.twin)) }
        if let x = try? readScreen(.twin) { screen[.twin] = x; rebaseScreen(.twin) }
        log("override", "twin", M("the owner's overrides on the twin: " + did.joined(separator: ", "), "переопределения владельца на двойнике: " + did.joined(separator: ", ")))
    }

    /// After writing to side O: its screen as it is now becomes its base (a new home city, a removed
    /// effect or a renumbered list can move its page); if its page had been the source's and moved,
    /// the source's page is put back.
    private func afterWrite(_ o: Side, from s: Side) throws {
        let before = screen[o]
        guard let now = try? readScreen(o) else { return }
        screen[o] = now; rebaseScreen(o)
        if let before, let src = screen[s], before.shown == src.shown, now.shown != src.shown {
            try mirrorScreen(from: s, fields: ["page"])
        }
    }

    private func firmwareRound() throws {
        for s in [Side.panel, .twin] {
            guard let f = fw[s], !f.id.isEmpty else { continue }
            guard let was = firmwareBase[s] else { firmwareBase[s] = f.id; continue }
            guard f.id != was else { continue }
            if f.state != "valid" {                                           // a new image confirms itself or rolls back
                fwWatch = Date().addingTimeInterval(15); continue
            }
            firmwareBase[s] = f.id; stateDirty = true
            caps[s] = nil
            log("note", "firmware", M("\(s.word.en) now runs \(f.id)", "\(s.on) теперь \(f.id)"))
            retryPending()
            if fw[s.other]?.id != f.id { try offerFirmware(from: s) }
        }
    }

    /// Side S's firmware to the other side: to the twin at once, to the panel only after a person's yes.
    private func offerFirmware(from s: Side) throws {
        let f = fw[s]!, o = s.other
        if o == .twin { try updateFirmware(from: s); return }
        if declined.contains(f.id) || offer?.id == f.id { return }
        let p = fw[.panel]!
        let source = capabilities(s).image ? M("read from the twin (/api/firmware/image)", "берётся с двойника (/api/firmware/image)")
                                           : M("the release v\(f.version) of \(repo) on GitHub", "выпуск v\(f.version) из \(repo) на GitHub")
        let o2 = Offer(id: f.id, version: f.version, build: f.build, panelVersion: p.version, panelBuild: p.build, source: source)
        offer = o2; offerFrom = s
        if testing, twinLike(name: p.name, mac: p.mac), UserDefaults.standard.bool(forKey: "syncAutoConfirmForTesting") {
            log("note", "firmware", M("the panel is a twin and syncAutoConfirmForTesting is on: answered Update by itself",
                                      "«панель» — двойник, и включён syncAutoConfirmForTesting: ответ «Обновить» дан автоматически"))
            locked { _firmwareAnswer = (f.id, true) }
            return
        }
        log("note", "firmware", M("asking whether to update the panel to \(f.id)", "спрашиваю, обновлять ли панель до \(f.id)"))
        DispatchQueue.main.async { self.askFirmware?(o2) }
    }

    private func answered(id: String, go: Bool) throws {
        guard let o = offer, o.id == id, let s = offerFrom else { return }
        offer = nil; offerFrom = nil
        guard go else { declined.insert(id); log("note", "firmware", M("the panel stays on its firmware (not now)", "панель остаётся на своей прошивке (не сейчас)")); return }
        guard fw[s]?.id == id else { log("note", "firmware", M("the twin's firmware changed meanwhile: not sent", "прошивка двойника за это время сменилась: не отправляю")); return }
        try updateFirmware(from: s)
    }

    /// The image of side S's firmware, sent to the other side and watched until it confirms itself.
    private func updateFirmware(from s: Side) throws {
        let f = fw[s]!, o = s.other
        phase(M("updating the firmware of \(o.word.en) to \(f.version)", "обновляю прошивку \(o.of) до \(f.version)"))
        let image: Data
        do { image = try firmwareImage(from: s, f) } catch let e as SyncError {
            noteOnce("fwimg-\(f.id)", M("the firmware \(f.id) cannot be carried: \(e.msg.en)", "прошивку \(f.id) не перенести: \(e.msg.ru)")); return
        }
        let free = fw[o]?.otaFree ?? 0
        guard free == 0 || image.count <= free else {
            noteOnce("fwfree-\(f.id)", M("the image (\(image.count) B) does not fit \(o.word.en)'s OTA slot (\(free) B)", "образ (\(image.count) Б) не помещается в OTA-раздел \(o.of) (\(free) Б)")); return
        }
        let a = try device(o).upload("/update", query: [], field: "firmware", filename: "firmware.bin", data: image, timeout: 300)
        guard a.status == 200 else { throw device(o).refused("POST /update", a) }
        log(s.arrow, "firmware", M("\(f.id) sent to \(o.word.en); it restarts and confirms the image within about a minute",
                                   "\(f.id) отправлена \(o == .panel ? "на панель: она перезагружается и подтверждает" : "двойнику: он перезагружается и подтверждает") образ примерно за минуту"))
        let was = fw[o]?.build ?? ""
        let deadline = Date().addingTimeInterval(300)
        Thread.sleep(forTimeInterval: 5)
        while Date() < deadline {
            Thread.sleep(forTimeInterval: 3)
            guard enabled else { throw SyncWait(M("sync was switched off while the firmware was being confirmed", "синхронизацию выключили, пока прошивка подтверждалась")) }
            guard let i = try? device(o).get("/api/info") else { continue }
            let n = Firmware(i)
            if n.version == f.version && n.build == f.build && n.state == "valid" {
                fw[o] = n; firmwareBase[o] = n.id; caps[o] = nil; stateDirty = true
                if let x = try? readScreen(o) { screen[o] = x; rebaseScreen(o) }
                log(s.arrow, "firmware", M("\(o.word.en) runs \(n.id): confirmed", "\(o.on) \(n.id): подтверждена"))
                if let x = try? readScreen(s) { screen[s] = x; rebaseScreen(s) }
                try? mirrorScreen(from: s, fields: ["page", "style", "bright", "off"])
                retryPending()                                        // it may give out scripts' sources now
                return
            }
            if n.build == was && !n.rolledBackFrom.isEmpty && n.state == "valid" {
                fw[o] = n; firmwareBase[o] = n.id
                throw SyncError(M("\(o.word.en) rolled back to \(n.id): the new image did not confirm itself", "\(o.on) откат на \(n.id): новый образ не подтвердил себя"))
            }
        }
        throw SyncError(M("\(o.word.en) did not confirm \(f.id) within five minutes", "\(o.on) за пять минут не подтвердилась \(f.id)"))
    }

    private func firmwareImage(from s: Side, _ f: Firmware) throws -> Data {
        if capabilities(s).image {
            let a = try device(s).firmwareImage()
            guard a.header("X-Firmware-Version") == f.version, a.data.count == f.bytes else {
                throw SyncError(M("the image read is \(a.data.count) B of \(a.header("X-Firmware-Version") ?? "?"), the device says \(f.bytes) B of \(f.version)",
                                  "прочитан образ \(a.data.count) Б версии \(a.header("X-Firmware-Version") ?? "?"), а устройство говорит \(f.bytes) Б версии \(f.version)"))
            }
            try checkImage(a.data)
            return a.data
        }
        guard let rel = try GitHub.release(tag: "v" + f.version) else {
            throw SyncError(M("\(s.word.en)'s firmware has no /api/firmware/image, and \(repo) has no release v\(f.version)",
                              "в прошивке \(s.of) нет /api/firmware/image, а в \(repo) нет выпуска v\(f.version)"))
        }
        guard rel.size == f.bytes else {
            throw SyncError(M("the build on \(s.word.en) (\(f.build), \(f.bytes) B) is not the release v\(f.version) of \(repo) (\(rel.size) B), and its firmware has no /api/firmware/image",
                              "сборка \(s.of) (\(f.build), \(f.bytes) Б) — не выпуск v\(f.version) из \(repo) (\(rel.size) Б), а /api/firmware/image в прошивке \(s.of) нет"))
        }
        let d = try GitHub.image(rel)
        try checkImage(d)
        return d
    }

    // MARK: kept between runs

    private struct Saved: Codable {
        var pair: String
        var settings: [String: [String: String]]
        var effects: [String: [String: String]]
        var firmware: [String: String]
        var pending: [String: [String: Int]]?        // effects not carried yet, by the side that has them
    }

    private func saveState() {
        guard aligned, !pairKey.isEmpty else { return }
        var s = Saved(pair: pairKey, settings: [:], effects: [:], firmware: [:], pending: [:])
        for side in [Side.panel, .twin] {
            s.settings[side.rawValue] = base[side]?["settings"] ?? [:]
            s.effects[side.rawValue] = base[side]?["effects"] ?? [:]
            s.firmware[side.rawValue] = firmwareBase[side] ?? ""
            s.pending?[side.rawValue] = pendingFx[side] ?? [:]
        }
        let enc = JSONEncoder(); enc.outputFormatting = [.sortedKeys]
        if let d = try? enc.encode(s), d != lastSaved { try? d.write(to: stateFile, options: .atomic); lastSaved = d }
        stateDirty = false
    }

    private func loadState() {
        guard let d = try? Data(contentsOf: stateFile), let s = try? JSONDecoder().decode(Saved.self, from: d), s.pair == pairKey else { return }
        for side in [Side.panel, .twin] {
            base[side, default: [:]]["settings"] = s.settings[side.rawValue] ?? [:]
            base[side, default: [:]]["effects"] = s.effects[side.rawValue] ?? [:]
            if let f = s.firmware[side.rawValue], !f.isEmpty { firmwareBase[side] = f }
            if let p = s.pending?[side.rawValue], !p.isEmpty { pendingFx[side] = p }
        }
        resumed = !(s.settings["panel"] ?? [:]).isEmpty
    }
}

/// The ESP application image's own checks: magic 0xE9, chip id 9 (ESP32-S3) at bytes 12-13, and when
/// byte 23 (hash_appended) is 1, the SHA-256 of everything before the last 32 bytes equals them
/// (esp_image_header_t; IDF 4.4 esp_image_format.c).
func checkImage(_ d: Data) throws {
    let b = [UInt8](d)
    guard b.count > 64, b[0] == 0xE9 else { throw SyncError(M("not an ESP application image", "это не образ приложения ESP")) }
    guard Int(b[12]) | Int(b[13]) << 8 == 9 else { throw SyncError(M("the image is not for the ESP32-S3", "образ не для ESP32-S3")) }
    if b[23] == 1 {
        let body = Data(b[0 ..< b.count - 32]), tail = Data(b[(b.count - 32)...])
        guard Data(SHA256.hash(data: body)) == tail else { throw SyncError(M("the image's own SHA-256 does not match: damaged", "собственный SHA-256 образа не сходится: образ повреждён")) }
    }
}
