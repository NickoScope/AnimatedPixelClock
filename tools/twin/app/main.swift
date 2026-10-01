// TWIN-NickoScopeMatrix-64x128: the virtual twin of the LED panel as a self-contained Mac app.
//
// Everything the twin needs is inside the bundle (Contents/Resources, put there by build.sh): the
// esp32sim engine, the ESP32-S3 mask ROM, the eFuse word, the firmware release and the web pages
// (the panel page and the project's web flasher with its navigator.serial shim). No Python, no
// repository. What the twin stores - its flash chip, the virtual access point, the home network it
// may join - lives in ~/Library/Application Support/TWIN-NickoScopeMatrix-64x128; a build with another
// bundle identifier (build.sh TWIN_BUNDLE_ID, a test build) keeps its own in
// ~/Library/Application Support/<its bundle identifier>, so it never runs the installed app's twin.
//
// The interface is English, or Russian (the EN · RU switch; the owner, 2026-09-30); the choice goes
// to the pages too (?lang=, localStorage twin-lang, window.twinSetLang).
//
// Opening the app starts the twin; quitting it, or closing its window, stops it (the owner,
// 2026-09-29: the twin runs only while it is worked with). A twin already running on the page's
// port - started from a terminal with tools/twin/twin.py - is shown as it is and left running.
//
// The launch is tools/twin/twin.py cmd_run's, ported: the same engine flags, the same home-network
// guard for the bridge (socket_vmnet, ADR-TWIN-02), Improv provisioning on a new chip. On the first
// start the twin of ~/twin/state (twin.py's) is copied in, so the app's twin continues from it.
//
// Sync with panel (SyncEngine.swift, the owner 2026-09-30): the twin and the physical panel mirror each
// other - screen, settings, Lua effects, firmware - while the switch is on. Switching it on takes two
// answers ("SYNC with two confirmations", the owner 11:19): "Enable sync with the panel <name, address,
// MAC>?", then the alignment with the list of differences; until both are given nothing is written.
// Firmware reaches the panel only after two more: "Update the panel too?" and "Really flash the physical
// panel <name> with <version>?". Cancel or Not now is the default button (Return) of all four; the
// buttons that write have no key. The consent is remembered with the pair's state while the switch stays on
// (1.4): a restart with the switch on asks nothing unless the twin changed meanwhile; switching off forgets it,
// quitting does not (SyncEngine.swift, markLaunch, quit). Off by default; the switch is remembered, and follows `defaults write
// <bundle id> syncEnabled -bool NO` from a terminal as well, at once, whatever window is open (a session
// that is about to flash the panel can switch sync off first). No window blocks the main dispatch queue:
// each is opened from the run loop (Controller.later), so the switch, SIGTERM and the status line are
// served while it is open.
//
// Instant (app 1.4, firmware 2.7.13): sync subscribes to both devices' events (UDP, one socket of the app); a twin
// without the home network sends them through the engine's forwarded UDP port (udpPort). Where they do not come, the
// twin's page tells the app of each gesture a person makes there (the message "twinInput" from panel.html) and its
// console of each effect that opens ("[luafx] open"): the screen round runs at once (SyncEngine.swift, "Instant events").
// App 1.4.1 (the owner's pair, 2026-10-01): a script's SHA-256 is read one at a time on a schedule, never in a batch, and
// the rounds that take a while serve the events between their requests (SyncEngine.swift: hashes, long); at each start of
// the twin fbAskHa is put off on it, whatever the switch says (holdAskHa, the owner's item 16).
//
// Settings (defaults write com.nickoscope.TWIN-NickoScopeMatrix-64x128 KEY VALUE, or -KEY VALUE on the
// command line): lang "en"|"ru",
// port 8790, httpPort 8080, udpPort 4210, serialPort 4000, cpi "2.45", lanSocket, dataDir, migrate
// (NO: never copy ~/twin/state in, for a test data directory);
// syncEnabled (NO), panelAddress ("" = found over mDNS), panelMac, syncSettingsEveryS (60),
// firmwareRepo ("NickoScope/AnimatedPixelClock": where releases and the gallery come from - the owner of
// another panel names their own fork). Test-only switches, honoured only when the "panel" has a twin's
// MAC and panelAddress names it by a loopback address: syncPanelMayBeTwinForTesting,
// syncConsentForTesting, syncAlignForTesting, syncAutoConfirmForTesting, syncAnswerDelayForTesting,
// syncEnableReturnForTesting, syncPressReturnForTesting, syncImageHoldForTesting (SyncEngine.swift).

import AppKit
import CryptoKit
import WebKit

// MARK: - language

let APP_NAME = "TWIN-NickoScopeMatrix-64x128"
let STANDARD_BUNDLE_ID = "com.nickoscope.TWIN-NickoScopeMatrix-64x128"
var LANG = "en"

/// syncEnabled as a key path, so the switch can follow a change made outside the app (KVO on defaults).
extension UserDefaults { @objc dynamic var syncEnabled: Bool { bool(forKey: "syncEnabled") } }
/// The English or the Russian text, by the EN · RU switch.
func L(_ en: String, _ ru: String) -> String { LANG == "ru" ? ru : en }

// MARK: - where things are

struct Paths {
    let res: URL, data: URL
    var engine: URL { res.appendingPathComponent("engine/esp32sim") }
    var rom: URL { res.appendingPathComponent("rom/esp32s3_rev0_rom.elf") }
    var efuse: URL { res.appendingPathComponent("efuse-opi.txt") }
    var image: URL { res.appendingPathComponent("firmware/merged.bin") }
    var web: URL { res.appendingPathComponent("web") }
    var flash: URL { data.appendingPathComponent("flash.bin") }
    var wifi: URL { data.appendingPathComponent("wifi.txt") }
    var lan: URL { data.appendingPathComponent("lan.txt") }
    var mac: URL { data.appendingPathComponent("mac.txt") }
    var named: URL { data.appendingPathComponent("named.txt") }
    var log: URL { data.appendingPathComponent("engine.log") }
    var firmwareVersion: String {
        (try? String(contentsOf: res.appendingPathComponent("firmware/VERSION"), encoding: .utf8))?
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? "?"
    }
}

func read(_ u: URL) -> String? { try? String(contentsOf: u, encoding: .utf8) }
func write(_ s: String, _ u: URL) { try? s.write(to: u, atomically: true, encoding: .utf8) }
func exists(_ u: URL) -> Bool { FileManager.default.fileExists(atPath: u.path) }

/// stdout of a system tool (route, arp, pgrep), "" if it fails.
func run(_ tool: String, _ args: [String]) -> String {
    let p = Process(); p.executableURL = URL(fileURLWithPath: tool); p.arguments = args
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
    do { try p.run() } catch { return "" }
    let d = pipe.fileHandleForReading.readDataToEndOfFile(); p.waitUntilExit()
    return String(data: d, encoding: .utf8) ?? ""
}

// MARK: - the network (twin.py lan_checks, default_gateway, cmd_lan_setup)

struct Gateway: Equatable { var interface = "", ip = "", mac = "" }

/// The default route's interface and gateway, and the gateway's MAC from the ARP table.
func defaultGateway() -> Gateway {
    var g = Gateway()
    for line in run("/sbin/route", ["-n", "get", "default"]).split(separator: "\n") {
        let f = line.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
        if f.count == 2 && f[0] == "gateway" { g.ip = f[1] }
        if f.count == 2 && f[0] == "interface" { g.interface = f[1] }
    }
    if !g.ip.isEmpty {
        let w = run("/usr/sbin/arp", ["-n", g.ip]).split(separator: " ").map(String.init)
        if let i = w.firstIndex(of: "at"), i + 1 < w.count {
            let o = w[i + 1].split(separator: ":")
            if o.count == 6, o.allSatisfy({ !$0.isEmpty && $0.count <= 2 }) {
                g.mac = o.map { String(format: "%02x", Int($0, radix: 16) ?? 0) }.joined(separator: ":")
            }
        }
    }
    return g
}

func recordedHome(_ p: Paths) -> Gateway? {
    guard let s = read(p.lan) else { return nil }
    var g = Gateway()
    for w in s.split(whereSeparator: { $0 == "\n" || $0 == " " }) {
        let kv = w.split(separator: "=", maxSplits: 1).map(String.init)
        guard kv.count == 2 else { continue }
        if kv[0] == "interface" { g.interface = kv[1] }
        if kv[0] == "gateway_ip" { g.ip = kv[1] }
        if kv[0] == "gateway_mac" { g.mac = kv[1] }
    }
    return g
}

/// The bridge daemon answers on its socket.
func socketAnswers(_ path: String) -> Bool {
    let fd = socket(AF_UNIX, SOCK_STREAM, 0); guard fd >= 0 else { return false }
    defer { close(fd) }
    var addr = sockaddr_un(); addr.sun_family = sa_family_t(AF_UNIX)
    let bytes = Array(path.utf8)
    guard bytes.count < MemoryLayout.size(ofValue: addr.sun_path) else { return false }
    withUnsafeMutableBytes(of: &addr.sun_path) { buf in for (i, b) in bytes.enumerated() { buf[i] = b } }
    return withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) == 0 }
    }
}

// MARK: - Improv and the virtual AP (twin.py improv_hex, wifi_spec)

func improvHex(_ ssid: String, _ pw: String) -> String {
    let s = Array(ssid.utf8), p = Array(pw.utf8)
    let rpc = [UInt8(s.count)] + s + [UInt8(p.count)] + p
    let data = [0x01, UInt8(rpc.count)] + rpc
    let pkt = Array("IMPROV".utf8) + [1, 3, UInt8(data.count)] + data
    let sum = pkt.reduce(0) { ($0 + Int($1)) & 0xFF }
    return (pkt + [UInt8(sum)]).map { String(format: "%02x", $0) }.joined()
}

func randomWord(_ n: Int) -> String {
    let a = Array("abcdefghjkmnpqrstuvwxyz23456789")
    return String((0..<n).map { _ in a.randomElement()! })
}

// MARK: - the twin

final class Twin {
    let p: Paths
    let port: Int, httpPort: Int, udpPort: Int, serialPort: Int, cpi: String, lanSocket: String
    private(set) var process: Process?
    private(set) var tail = ""
    private(set) var onLan = false
    private(set) var why = ""            // why the twin is not on the home network, if it is not
    private var logHandle: FileHandle?
    var onChange: (() -> Void)?
    /// Each whole line of the twin's console, on the pipe's own queue (Sync with panel: "[luafx] open <name>").
    var onConsoleLine: ((String) -> Void)?
    private var partial = ""

    init(paths: Paths, defaults d: UserDefaults) {
        p = paths
        port = d.integer(forKey: "port"); httpPort = d.integer(forKey: "httpPort"); udpPort = d.integer(forKey: "udpPort")
        serialPort = d.integer(forKey: "serialPort")
        cpi = d.string(forKey: "cpi") ?? "2.45"; lanSocket = d.string(forKey: "lanSocket") ?? ""
    }

    var pageURL: URL { URL(string: "http://127.0.0.1:\(port)/panel.html")! }
    var flasherURL: URL { URL(string: "http://127.0.0.1:\(port)/flasher/")! }
    var ours: Bool { process?.isRunning == true }
    var portalURL: URL? {
        if onLan, let a = address { return URL(string: "http://\(a)/") }
        return ours ? URL(string: "http://127.0.0.1:\(httpPort)/") : nil
    }

    /// Where the twin's own web server answers: its IP on the home network, or the port forwarded to
    /// 127.0.0.1 without it.
    var apiAddress: String? { ours ? (onLan ? address : "127.0.0.1:\(httpPort)") : nil }

    /// The twin's address on the home network, from its console ("IP Address: ...").
    var address: String? {
        guard let r = tail.range(of: "IP Address: ", options: .backwards) else { return nil }
        let a = String(tail[r.upperBound...].prefix { "0123456789.".contains($0) })
        return a.isEmpty ? nil : a
    }

    func answers(_ done: @escaping (Bool) -> Void) {
        var req = URLRequest(url: pageURL); req.timeoutInterval = 1.5
        URLSession.shared.dataTask(with: req) { _, r, _ in done((r as? HTTPURLResponse)?.statusCode == 200) }.resume()
    }

    /// The data directory: made on the first start, with twin.py's twin copied in if there is one
    /// and nothing runs on it; otherwise a new chip with its own MAC and access point.
    func prepare(migrate: Bool = true) -> String? {
        let fm = FileManager.default
        try? fm.createDirectory(at: p.data, withIntermediateDirectories: true)
        guard !exists(p.flash) else { return nil }
        let old = fm.homeDirectoryForCurrentUser.appendingPathComponent("twin/state")
        let oldFlash = old.appendingPathComponent("flash.bin")
        if migrate && exists(oldFlash) {
            if !run("/usr/bin/pgrep", ["-f", oldFlash.path]).isEmpty {
                return L("The twin of ~/twin/state is running (it was started from a terminal). Stop it, and the app will take it over.",
                         "Двойник из ~/twin/state сейчас работает (его запустили из терминала). Остановите его, и приложение перенесёт его к себе.")
            }
            for name in ["flash.bin", "wifi.txt", "lan.txt"] {
                let src = old.appendingPathComponent(name)
                if exists(src) { try? fm.copyItem(at: src, to: p.data.appendingPathComponent(name)) }
            }
            write("02:54:57:49:4E:01\n", p.mac)       // twin.py's MAC: the same twin, the same address lease
            write("copied from ~/twin/state\n", p.named)
            return nil
        }
        // A new twin on this Mac: its own locally administered MAC ("TWI" + two random bytes), so
        // twins on two Macs of one network never collide, and its own virtual access point.
        if !exists(p.mac) {
            write(String(format: "02:54:57:49:%02X:%02X\n", Int.random(in: 0...255), Int.random(in: 0...255)), p.mac)
        }
        if !exists(p.wifi) { write("TWIN-AP\n\(randomWord(12))\n", p.wifi) }
        return nil
    }

    /// Whether the twin may go on the home network, and if not, why (twin.py lan_checks).
    func lanDecision() -> (Bool, String) {
        if lanSocket.isEmpty || !FileManager.default.fileExists(atPath: lanSocket) {
            return (false, L("no socket_vmnet service: the twin has no address of its own on the network (see \"How to enable the home network\" in the menu)",
                              "нет службы socket_vmnet — двойник без своего адреса в сети (см. «Как включить домашнюю сеть» в меню)"))
        }
        if !socketAnswers(lanSocket) { return (false, L("the socket_vmnet service does not answer", "служба socket_vmnet не отвечает")) }
        guard let home = recordedHome(p) else { return (false, L("this network is not marked as home (menu: \"This is my home network\")", "эта сеть не отмечена как домашняя (меню: «Это моя домашняя сеть»)")) }
        if home != defaultGateway() { return (false, L("not the home network: the twin stays off it", "это не домашняя сеть — двойник в неё не выходит")) }
        if !exists(p.named) { return (false, L("the twin is not named TWIN-… yet", "двойник ещё не назван TWIN-…")) }
        return (true, "")
    }

    func start() {
        guard process?.isRunning != true else { return }
        let wifi = (read(p.wifi) ?? "").split(separator: "\n", omittingEmptySubsequences: false).map(String.init)
        let ssid = wifi.count > 0 ? wifi[0] : "", pw = wifi.count > 1 ? wifi[1] : ""
        let newChip = !exists(p.flash)
        (onLan, why) = lanDecision()
        var args = ["--board", "panel", "--boot", "rom", "--rom", p.rom.path]
        if newChip { args += ["--flash-image", p.image.path] }       // an existing flash file wins anyway
        args += ["--flash-mb", "32", "--flash-id", "c28039", "--psram-mb", "16", "--efuse-regs", p.efuse.path,
                 "--console", "usb", "--no-dump", "--flash-persist", p.flash.path,
                 "--mac", (read(p.mac) ?? "02:54:57:49:4E:01").trimmingCharacters(in: .whitespacesAndNewlines),
                 "--cpi", cpi, "--web", String(port), "--web-dir", p.web.path]
        // The USB port as RFC 2217 on 127.0.0.1, for esptool, PlatformIO and a serial monitor:
        // rfc2217://127.0.0.1:<serialPort> (esp-soc/src/rfc2217.rs).
        if serialPort > 0 { args += ["--serial-tcp", String(serialPort)] }
        if !ssid.isEmpty && !ssid.contains(",") && !pw.contains(",") {
            args += ["--wifi", "ssid=\(ssid)" + (pw.isEmpty ? "" : ",psk=\(pw)")]
            args += onLan ? ["--net", "bridge:\(lanSocket)"] : ["--hostfwd", "tcp:\(httpPort)-80", "--hostfwd", "udp:\(udpPort)-4210"]
            if newChip { args += ["--serial-hex", improvHex(ssid, pw)] }   // a new chip learns the AP over Improv
        }
        tail = ""
        FileManager.default.createFile(atPath: p.log.path, contents: nil)
        logHandle = try? FileHandle(forWritingTo: p.log)
        let proc = Process()
        proc.executableURL = p.engine; proc.arguments = args
        let pipe = Pipe(); proc.standardOutput = pipe; proc.standardError = pipe
        partial = ""
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty else { return }
            self?.logHandle?.write(d)
            let s = String(decoding: d, as: UTF8.self)
            if let self, let tell = self.onConsoleLine {
                // Whole lines only: a chunk ends anywhere. The rest waits for the next chunk (4 KB at most).
                let lines = (self.partial + s).split(separator: "\n", omittingEmptySubsequences: false)
                self.partial = String(lines.last ?? "").suffix(4096).description
                for l in lines.dropLast() { tell(String(l)) }
            }
            DispatchQueue.main.async {
                guard let self else { return }
                self.tail += s
                if self.tail.count > 200_000 { self.tail = String(self.tail.suffix(100_000)) }
                self.onChange?()
            }
        }
        proc.terminationHandler = { [weak self] _ in DispatchQueue.main.async { self?.onChange?() } }
        do { try proc.run(); process = proc } catch { tail = L("The engine did not start: ", "Движок не запустился: ") + "\(error.localizedDescription)\n" }
        onChange?()
    }

    func stop() {
        guard let proc = process, proc.isRunning else { return }
        proc.terminate()
        let deadline = Date().addingTimeInterval(10)
        while proc.isRunning && Date() < deadline { usleep(100_000) }
        if proc.isRunning { kill(proc.processIdentifier, SIGKILL) }
        try? logHandle?.close(); logHandle = nil
    }

    /// A new twin is named TWIN-… over its own API before it may go on the home network, so the router
    /// and HA can tell it from the panel (twin.py lan_checks; the firmware's POST /api/rename).
    func nameIfNeeded(_ done: @escaping (Bool) -> Void) {
        guard !exists(p.named), let base = URL(string: "http://127.0.0.1:\(httpPort)") else { done(false); return }
        URLSession.shared.dataTask(with: base.appendingPathComponent("api/info")) { [self] d, _, _ in
            let info = d.flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
            guard let info else { done(false); return }
            if let n = info["deviceName"] as? String, n.hasPrefix("TWIN-") { write("\(n)\n", p.named); done(true); return }
            // The firmware takes 1-31 letters, digits and hyphens (web.cpp handleRename): the panel's name
            // with the MAC's last byte instead of its "01" fits exactly.
            let tailMac = (read(p.mac) ?? "").replacingOccurrences(of: ":", with: "").trimmingCharacters(in: .whitespacesAndNewlines).suffix(2)
            var req = URLRequest(url: base.appendingPathComponent("api/rename")); req.httpMethod = "POST"
            req.setValue("application/json", forHTTPHeaderField: "Content-Type")
            let name = "TWIN-NickoScopeMatrix-64x128-\(tailMac)"
            req.httpBody = try? JSONSerialization.data(withJSONObject: ["name": name])
            URLSession.shared.dataTask(with: req) { _, r, _ in
                let ok = (r as? HTTPURLResponse)?.statusCode == 200
                if ok { write("\(name)\n", self.p.named) }
                done(ok)
            }.resume()
        }.resume()
    }
}

// MARK: - GitHub (tools/agent/update.py)

struct Failure: Error, CustomStringConvertible { let description: String; init(_ s: String) { description = s } }

/// GET with a size limit, synchronously (called off the main thread). HTTPS stays HTTPS.
func fetch(_ url: String, limit: Int, timeout: TimeInterval = 30) throws -> Data {
    var req = URLRequest(url: URL(string: url)!); req.timeoutInterval = timeout
    req.setValue("ledmatrix-twin-app", forHTTPHeaderField: "User-Agent")
    req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    let sem = DispatchSemaphore(value: 0)
    var out: Result<Data, Error> = .failure(Failure("\(url): no answer"))
    URLSession.shared.dataTask(with: req) { d, r, e in
        if let e { out = .failure(e) }
        else if url.hasPrefix("https://"), r?.url?.scheme != "https" { out = .failure(Failure("refused a non-HTTPS redirect")) }
        else if let h = r as? HTTPURLResponse, h.statusCode != 200 { out = .failure(Failure("\(url): HTTP \(h.statusCode)")) }
        else if let d, d.count <= limit { out = .success(d) }
        else { out = .failure(Failure("\(url): larger than \(limit) bytes")) }
        sem.signal()
    }.resume()
    sem.wait()
    return try out.get()
}

enum GitHub {
    static let defaultRepo = "NickoScope/AnimatedPixelClock"
    /// Where releases and the gallery come from: the owner's fork by default, another owner's own fork
    /// by the setting firmwareRepo (menu Sync: "Firmware releases from...").
    static var repo: String {
        let r = (UserDefaults.standard.string(forKey: "firmwareRepo") ?? "").trimmingCharacters(in: .whitespaces)
        return validRepo(r) ? r : defaultRepo
    }
    struct Release { let tag: String, notes: String, asset: String, assetName: String, size: Int, sums: String }

    static func ver(_ tag: String) -> [Int] {
        let t = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        let parts = t.split(separator: ".").prefix(3).map { Int($0.prefix { $0.isNumber }) ?? -1 }
        return parts.count == 3 && !parts.contains(-1) ? parts : []
    }

    /// A published release that carries the board's OTA image and its checksums, or nil.
    static func parse(_ r: [String: Any]) -> Release? {
        guard r["draft"] as? Bool != true, r["prerelease"] as? Bool != true,
              let tag = r["tag_name"] as? String, !ver(tag).isEmpty else { return nil }
        let assets = Dictionary((r["assets"] as? [[String: Any]] ?? []).compactMap { a in (a["name"] as? String).map { ($0, a) } },
                                uniquingKeysWith: { a, _ in a })
        let name = "OTA_ONLY_firmware-\(tag)-waveshare.bin"
        // Both files from the repository's own releases: nothing else is downloaded.
        let prefix = "https://github.com/\(repo)/releases/download/"
        guard let ota = assets[name], let url = ota["browser_download_url"] as? String, url.hasPrefix(prefix),
              let sums = assets["SHA256SUMS.txt"]?["browser_download_url"] as? String, sums.hasPrefix(prefix) else { return nil }
        return Release(tag: tag, notes: r["body"] as? String ?? "", asset: url, assetName: name, size: ota["size"] as? Int ?? -1, sums: sums)
    }

    /// The newest published release of the fork that carries the board's OTA image and its checksums.
    static func latest() throws -> Release? {
        let list = try JSONSerialization.jsonObject(with: try fetch("https://api.github.com/repos/\(repo)/releases?per_page=100", limit: 4 << 20)) as? [[String: Any]] ?? []
        var best: Release?
        for r in list {
            guard let rel = parse(r) else { continue }
            if best == nil || ver(rel.tag).lexicographicallyPrecedes(ver(best!.tag)) == false { best = rel }
        }
        return best
    }

    /// The release with this tag ("v2.7.7"), or nil when the fork has none (Sync with panel: the image of a
    /// device whose firmware cannot give out its own).
    static func release(tag: String) throws -> Release? {
        guard tag.range(of: #"^v[0-9]+\.[0-9]+\.[0-9]+$"#, options: .regularExpression) != nil else { return nil }
        let d: Data
        do { d = try fetch("https://api.github.com/repos/\(repo)/releases/tags/\(tag)", limit: 4 << 20) }
        catch let f as Failure where f.description.hasSuffix("HTTP 404") { return nil }
        return parse(try JSONSerialization.jsonObject(with: d) as? [String: Any] ?? [:])
    }

    static func image(_ r: Release) throws -> Data {
        let data = try fetch(r.asset, limit: 8 << 20, timeout: 180)
        guard data.count == r.size else { throw Failure(L("downloaded \(data.count) bytes, the release says \(r.size)", "скачано \(data.count) байт, а в выпуске \(r.size)")) }
        let sums = String(decoding: try fetch(r.sums, limit: 64 << 10), as: UTF8.self)
        let want = sums.split(separator: "\n").compactMap { line -> String? in
            let f = line.split(separator: " ", omittingEmptySubsequences: true)
            return f.count == 2 && f[1].trimmingCharacters(in: CharacterSet(charactersIn: "*")) == r.assetName ? f[0].lowercased() : nil
        }.first
        let got = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard let want else { throw Failure(L("\(r.assetName) is not in SHA256SUMS.txt", "\(r.assetName) нет в SHA256SUMS.txt")) }
        guard got == want else { throw Failure(L("SHA-256 mismatch: \(got), the release says \(want)", "SHA-256 не совпадает: \(got) против \(want)")) }
        guard data.first == 0xE9 else { throw Failure(L("not an ESP application image", "это не образ приложения ESP")) }
        guard data.count > 14, Int(data[12]) | Int(data[13]) << 8 == 9 else { throw Failure(L("the image is not for the ESP32-S3", "образ не для ESP32-S3")) }
        do { try checkImage(data) } catch let e as SyncError { throw Failure(L(e.msg.en, e.msg.ru)) }   // its own appended SHA-256
        return data
    }

    /// POST /update, multipart, field "firmware" - the portal's Firmware card (update.py upload).
    static func upload(_ image: Data, to addr: String) throws {
        let b = "----twinota" + randomWord(16)
        var body = Data("--\(b)\r\nContent-Disposition: form-data; name=\"firmware\"; filename=\"firmware.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n".utf8)
        body.append(image); body.append(Data("\r\n--\(b)--\r\n".utf8))
        var req = URLRequest(url: URL(string: "http://\(addr)/update")!); req.httpMethod = "POST"; req.timeoutInterval = 300
        req.setValue("multipart/form-data; boundary=\(b)", forHTTPHeaderField: "Content-Type")
        let sem = DispatchSemaphore(value: 0); var status = 0; var err: Error?
        URLSession.shared.uploadTask(with: req, from: body) { _, r, e in err = e; status = (r as? HTTPURLResponse)?.statusCode ?? 0; sem.signal() }.resume()
        sem.wait()
        if let err { throw err }
        guard status == 200 else { throw Failure(L("\(addr) answered /update with HTTP \(status)", "\(addr) ответил на /update: HTTP \(status)")) }
    }
}

// MARK: - the window

/// The panel page's message "twinInput" (a person's gesture there): its text, to SyncEngine.twinPageInput.
final class PageInput: NSObject, WKScriptMessageHandler {
    let tell: (String) -> Void
    init(_ tell: @escaping (String) -> Void) { self.tell = tell }
    func userContentController(_ c: WKUserContentController, didReceive m: WKScriptMessage) {
        guard let what = m.body as? String, what.count <= 16 else { return }
        tell(what)
    }
}

final class Controller: NSObject, NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate {
    var window: NSWindow!
    var web: WKWebView!
    let status = NSTextField(labelWithString: "")
    let langSwitch = NSSegmentedControl(labels: ["EN", "RU"], trackingMode: .selectOne, target: nil, action: nil)
    var buttons: [(NSButton, () -> String)] = []
    var twin: Twin!
    var shown = false, polls = 0
    /// Each start of the twin: the run of holdAskHa that is current (an older one stops).
    var askHaRun = 0
    var lastStatus: () -> String = { "" }
    // Sync with panel
    var sync: SyncEngine!
    let finder = PanelFinder()
    let syncToggle = NSButton(checkboxWithTitle: "", target: nil, action: nil)
    let syncStatus = NSTextField(labelWithString: "")
    var syncButtons: [(NSButton, () -> String)] = []
    var twinMac = ""
    var syncSwitchWatch: NSKeyValueObservation?
    /// The sync question on screen, if any: closed (as Cancel, Not now) when sync is switched off meanwhile.
    var syncQuestion: NSAlert?

    func applicationDidFinishLaunching(_ n: Notification) {
        let d = UserDefaults.standard
        // The installed app's data, or a test build's own (its bundle identifier): a test build never
        // takes the installed app's flash, MAC and sync state, and so never its pair with the panel.
        let bundleId = Bundle.main.bundleIdentifier ?? STANDARD_BUNDLE_ID
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(bundleId == STANDARD_BUNDLE_ID ? APP_NAME : bundleId)
        d.register(defaults: ["port": 8790, "httpPort": 8080, "udpPort": 4210, "serialPort": 4000, "cpi": "2.45", "lang": "en",
                              "lanSocket": "/var/run/socket_vmnet.bridged.en0", "dataDir": support.path, "migrate": bundleId == STANDARD_BUNDLE_ID,
                              "syncEnabled": false, "panelAddress": "", "panelMac": "", "firmwareRepo": GitHub.defaultRepo,
                              "syncSettingsEveryS": 60, "syncPanelMayBeTwinForTesting": false, "syncAutoConfirmForTesting": false,
                              "syncConsentForTesting": false, "syncAlignForTesting": "", "syncAnswerDelayForTesting": 0,
                              "syncEnableReturnForTesting": 0, "syncPressReturnForTesting": 0, "syncImageHoldForTesting": 0])
        LANG = d.string(forKey: "lang") == "ru" ? "ru" : "en"
        let paths = Paths(res: Bundle.main.resourceURL!, data: URL(fileURLWithPath: d.string(forKey: "dataDir")!))
        twin = Twin(paths: paths, defaults: d)
        twin.onChange = { [weak self] in self?.refresh() }
        sync = SyncEngine(dataDir: paths.data)
        // A person at the twin, told at once: an effect opening in its console, a gesture on its page (twinInput) - the
        // screen round runs then rather than within 3 s, where the twin's own instant events do not come.
        let engine = sync!
        twin.onConsoleLine = { line in engine.twinConsoleLine(line) }
        sync.setLanguage(LANG)
        sync.onStatus = { [weak self] in self?.showSyncStatus() }
        // A question is a modal alert. It is opened from the run loop, not from a block on the main dispatch
        // queue: runModal inside such a block holds that serial queue for as long as the question is open,
        // and nothing else sent to it runs meanwhile - the status line, the switch followed from defaults,
        // SIGTERM. In the default mode only: a second question waits until the first one is answered.
        sync.askConsent = { [weak self] c in Controller.later { self?.askConsent(c) } }
        sync.askDirection = { [weak self] s in Controller.later { self?.askDirection(s) } }
        sync.askResume = { [weak self] s in Controller.later { self?.askResume(s) } }
        sync.askFirmware = { [weak self] o in Controller.later { self?.askFirmware(o) } }
        sync.onPanelLost = { [weak self] in self?.finder.refresh() }
        finder.onChange = { [weak self] f in self?.sync.setFound(f) }
        buildMenu(); buildWindow()
        // Whether the switch is on as the app starts: only then does a consent kept in sync-state.json count (the
        // owner's item 15, 2026-10-01: a restart with the switch on asks nothing, unless the twin changed meanwhile).
        sync.markLaunch(on: d.bool(forKey: "syncEnabled"))
        sync.start()
        if d.bool(forKey: "syncEnabled") { enableSync(true) } else { showSyncStatus() }
        // The switch follows the setting when something else changes it (defaults write from a terminal).
        // Off takes effect here and now, on whatever thread this is called: the engine's switch has its own
        // lock, and a session about to flash the panel must not wait for the main thread. The window follows.
        syncSwitchWatch = d.observe(\.syncEnabled, options: [.new]) { [weak self] _, _ in
            guard let self else { return }
            let on = UserDefaults.standard.bool(forKey: "syncEnabled")
            if !on { self.sync.setEnabled(false) }
            DispatchQueue.main.async {
                if on != (self.syncToggle.state == .on) || on != self.sync.enabled { self.enableSync(on) }
            }
        }
        twin.answers { [weak self] up in DispatchQueue.main.async {
            guard let self else { return }
            if !up {
                if let problem = self.twin.prepare(migrate: d.bool(forKey: "migrate")) { self.say { problem }; self.showText(problem); return }
                self.twin.start()
            }
            self.poll()
        } }
    }

    func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 820),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        // A build with another bundle identifier (build.sh TWIN_BUNDLE_ID, for tests) says so in its title,
        // so its window is never taken for the installed app's twin.
        let standard = Bundle.main.bundleIdentifier == STANDARD_BUNDLE_ID
        window.title = standard ? APP_NAME : "\(APP_NAME) — TEST \(Bundle.main.bundleIdentifier ?? "?") · \(twin.p.data.path)"
        window.setFrameAutosaveName("TwinPanelWindow")
        window.delegate = self
        let config = WKWebViewConfiguration()
        // The panel page posts "twinInput" for each gesture a person makes there (the knob turned or pressed, a remote's
        // button, BOOT, RESET: ~/twin/esp32sim web/panel.html tellApp). A proxy holds the handler: the content
        // controller keeps it strongly.
        config.userContentController.add(PageInput { [weak self] what in self?.sync.twinPageInput(what) }, name: "twinInput")
        web = WKWebView(frame: .zero, configuration: config)
        web.navigationDelegate = self; web.uiDelegate = self
        web.setValue(false, forKey: "drawsBackground")
        func button(_ label: @escaping () -> String, _ a: Selector) -> NSButton {
            let b = NSButton(title: label(), target: self, action: a); b.bezelStyle = .rounded
            buttons.append((b, label)); return b
        }
        langSwitch.selectedSegment = LANG == "ru" ? 1 : 0
        langSwitch.target = self; langSwitch.action = #selector(switchLanguage)
        langSwitch.toolTip = "English · Русский"
        let bar = NSStackView(views: [button({ L("Panel", "Панель") }, #selector(showPanel)),
                                      button({ L("Portal ↗", "Портал ↗") }, #selector(openPortal)),
                                      button({ L("Flasher ↗", "Прошивальщик ↗") }, #selector(openFlasher)),
                                      button({ L("Restart", "Перезапустить") }, #selector(restart)),
                                      langSwitch, status])
        bar.orientation = .horizontal; bar.spacing = 8
        bar.edgeInsets = NSEdgeInsets(top: 6, left: 10, bottom: 6, right: 10)
        status.lineBreakMode = .byTruncatingTail; status.textColor = .secondaryLabelColor
        // The second row: Sync with panel - the switch, which panel, what it did last.
        syncToggle.title = L("Sync with panel", "Синхронизация с панелью")
        syncToggle.target = self; syncToggle.action = #selector(syncSwitched)
        let which = NSButton(title: L("Which panel…", "Какая панель…"), target: self, action: #selector(choosePanel)); which.bezelStyle = .rounded
        syncButtons = [(which, { L("Which panel…", "Какая панель…") })]
        syncStatus.lineBreakMode = .byTruncatingTail; syncStatus.textColor = .secondaryLabelColor
        syncStatus.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let syncBar = NSStackView(views: [syncToggle, which, syncStatus])
        syncBar.orientation = .horizontal; syncBar.spacing = 8
        syncBar.edgeInsets = NSEdgeInsets(top: 0, left: 10, bottom: 6, right: 10)
        let root = NSStackView(views: [bar, syncBar, web])
        root.orientation = .vertical; root.spacing = 0; root.alignment = .leading
        web.translatesAutoresizingMaskIntoConstraints = false
        root.addConstraints([web.widthAnchor.constraint(equalTo: root.widthAnchor)])
        window.contentView = root
        window.center()
        // A test build never takes the focus: the owner may be working meanwhile (2026-09-30, a sync test's
        // window taking the focus was closed by him). It opens behind the others, not activated.
        if standard { window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true) } else { window.orderBack(nil) }
        say { L("Starting the twin…", "Запускаю двойника…") }
    }

    func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let m = NSMenu()
        m.addItem(withTitle: L("About \(APP_NAME)", "О программе \(APP_NAME)"), action: #selector(about), keyEquivalent: "")
        m.addItem(.separator())
        m.addItem(withTitle: L("Restart the twin", "Перезапустить двойника"), action: #selector(restart), keyEquivalent: "r")
        m.addItem(withTitle: L("Update the firmware from GitHub…", "Обновить прошивку с GitHub…"), action: #selector(updateFromGitHub), keyEquivalent: "u")
        m.addItem(withTitle: L("For developers: SDK, esptool, PlatformIO…", "Для разработчиков: SDK, esptool, PlatformIO…"), action: #selector(forDevelopers), keyEquivalent: "")
        m.addItem(withTitle: L("This is my home network", "Это моя домашняя сеть"), action: #selector(markHome), keyEquivalent: "")
        m.addItem(withTitle: L("How to enable the home network…", "Как включить домашнюю сеть…"), action: #selector(howLan), keyEquivalent: "")
        m.addItem(.separator())
        let lang = NSMenuItem(title: L("Language: Русский", "Язык: English"), action: #selector(toggleLanguage), keyEquivalent: "")
        m.addItem(lang)
        m.addItem(.separator())
        m.addItem(withTitle: L("Show the twin's data in Finder", "Показать данные двойника в Finder"), action: #selector(showData), keyEquivalent: "")
        m.addItem(withTitle: L("The twin's log", "Журнал двойника"), action: #selector(showLog), keyEquivalent: "l")
        m.addItem(.separator())
        m.addItem(withTitle: L("Hide", "Скрыть"), action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        m.addItem(withTitle: L("Quit and stop the twin", "Выйти и остановить двойника"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = m
        let syncItem = NSMenuItem(); main.addItem(syncItem)
        let sm = NSMenu(title: L("Sync", "Синхронизация"))
        let on = NSMenuItem(title: L("Sync with panel", "Синхронизация с панелью"), action: #selector(toggleSync), keyEquivalent: "y")
        on.state = UserDefaults.standard.bool(forKey: "syncEnabled") ? .on : .off
        sm.addItem(on)
        sm.addItem(withTitle: L("Sync now", "Синхронизировать сейчас"), action: #selector(syncNow), keyEquivalent: "")
        sm.addItem(.separator())
        sm.addItem(withTitle: L("Which panel…", "Какая панель…"), action: #selector(choosePanel), keyEquivalent: "")
        sm.addItem(withTitle: L("Firmware releases from…", "Выпуски прошивки из…"), action: #selector(chooseRepo), keyEquivalent: "")
        sm.addItem(.separator())
        sm.addItem(withTitle: L("The sync log", "Журнал синхронизации"), action: #selector(showSyncLog), keyEquivalent: "")
        sm.addItem(withTitle: L("How sync works…", "Как работает синхронизация…"), action: #selector(syncHelp), keyEquivalent: "")
        syncItem.submenu = sm
        let editItem = NSMenuItem(); main.addItem(editItem)
        let e = NSMenu(title: L("Edit", "Правка"))
        e.addItem(withTitle: L("Copy", "Копировать"), action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        e.addItem(withTitle: L("Paste", "Вставить"), action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        e.addItem(withTitle: L("Select All", "Выделить всё"), action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = e
        let winItem = NSMenuItem(); main.addItem(winItem)
        let w = NSMenu(title: L("Window", "Окно"))
        w.addItem(withTitle: L("Minimize", "Свернуть"), action: #selector(NSWindow.miniaturize(_:)), keyEquivalent: "m")
        winItem.submenu = w
        NSApp.mainMenu = main
    }

    // MARK: language

    @objc func switchLanguage() { setLanguage(langSwitch.selectedSegment == 1 ? "ru" : "en") }
    @objc func toggleLanguage() { setLanguage(LANG == "ru" ? "en" : "ru") }

    /// The app's own texts at once, and the page's through window.twinSetLang (the pages' contract:
    /// ?lang=, localStorage twin-lang), so nothing reloads and the twin is not disturbed.
    func setLanguage(_ lang: String) {
        LANG = lang
        UserDefaults.standard.set(lang, forKey: "lang")
        langSwitch.selectedSegment = lang == "ru" ? 1 : 0
        for (b, label) in buttons + syncButtons { b.title = label() }
        syncToggle.title = L("Sync with panel", "Синхронизация с панелью")
        sync.setLanguage(lang)
        buildMenu()
        say(lastStatus)
        showSyncStatus()
        web.evaluateJavaScript("try{localStorage.setItem('twin-lang','\(lang)')}catch(e){}; window.twinSetLang && window.twinSetLang('\(lang)')", completionHandler: nil)
    }

    func withLang(_ u: URL) -> URL {
        var c = URLComponents(url: u, resolvingAgainstBaseURL: false)!
        c.queryItems = (c.queryItems ?? []).filter { $0.name != "lang" } + [URLQueryItem(name: "lang", value: LANG)]
        return c.url!
    }

    // MARK: the twin's state

    func poll() {
        twin.answers { [weak self] up in DispatchQueue.main.async {
            guard let self else { return }
            if up {
                if !self.shown { self.shown = true; self.showPanel(); self.afterStart(); self.holdAskHa() }
                self.refresh(); return
            }
            self.polls += 1
            if let p = self.twin.process, !p.isRunning {
                let code = p.terminationStatus
                self.say { L("The twin did not start (code \(code)).", "Двойник не запустился (код \(code)).") }
                self.showText(L("The engine stopped:", "Движок завершился:") + "\n\n" + self.twin.tail); return
            }
            if self.polls % 4 == 0 { self.showText(L("Starting the twin…", "Запуск двойника…") + "\n\n" + self.twin.tail) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.poll() }
        } }
    }

    /// A new twin: once its firmware answers, name it TWIN-… and, if this is the home network,
    /// start it again on it.
    func afterStart() {
        guard twin.ours, !twin.onLan, !exists(twin.p.named) else { return }
        func tryName(_ left: Int) {
            twin.nameIfNeeded { [weak self] ok in DispatchQueue.main.async {
                guard let self else { return }
                if ok { if self.twin.lanDecision().0 { self.restart() } else { self.refresh() }; return }
                if left > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + 5) { tryName(left - 1) } }
            } }
        }
        tryName(36)    // the firmware's web server comes up after Wi-Fi and Improv: up to three minutes
    }

    /// Each start of the twin by this app (the owner's item 16, 2026-10-01): fbAskHa off on it, whatever the switch of
    /// Sync with panel says (SyncEngine.askHaOffAtStart) - so a twin without an AeroAPI key of its own never has Home
    /// Assistant fetch a flight board with the owner's key, also while sync is off (sync keeps it off only while it is on:
    /// enforceOverrides). Only where its firmware has the setting (2.7.13: fbAskHa in /api/export). Like the name
    /// (afterStart), once its firmware's web server answers: every 5 s, three minutes at most; a restart starts it again.
    func holdAskHa() {
        askHaRun += 1
        let run = askHaRun
        func again(_ left: Int) { if left > 0 { DispatchQueue.main.asyncAfter(deadline: .now() + 5) { attempt(left - 1) } } }
        func attempt(_ left: Int) {
            guard run == askHaRun, twin.ours else { return }
            guard let addr = twin.apiAddress else { again(left); return }          // on the home network: its address not known yet
            let mac = (read(twin.p.mac) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            DispatchQueue.global(qos: .utility).async { [weak self] in
                let r = SyncEngine.askHaOffAtStart(Device(.twin, addr), mac: mac)
                DispatchQueue.main.async {
                    guard let self, run == self.askHaRun else { return }
                    switch r {
                    case .notUp: again(left)
                    case .set: self.sync.note("override", M("the twin started: fbAskHa off on it - the owner's override, whatever the switch of sync",
                                                            "двойник запущен: fbAskHa на нём выключен — переопределение владельца, независимо от синхронизации"))
                    case .already: self.sync.note("override", M("the twin started: fbAskHa is off on it already", "двойник запущен: fbAskHa на нём уже выключен"))
                    case .noSetting: self.sync.note("override", M("the twin started: its firmware has no fbAskHa (before 2.7.13) - nothing to set", "двойник запущен: в его прошивке нет fbAskHa (до 2.7.13) — выключать нечего"))
                    case .notThisTwin where left > 0: again(left)                    // an address not settled yet: looked at again
                    case .notThisTwin(let m): self.sync.note("override", M("\(addr) answers with MAC \(m), not this twin's \(mac): fbAskHa not written", "\(addr) отвечает с MAC \(m), а не этого двойника \(mac): fbAskHa не пишу"))
                    case .refused(let why): self.sync.note("override", M("the twin started: fbAskHa could not be switched off - \(why)", "двойник запущен: fbAskHa выключить не удалось — \(why)"))
                    }
                }
            }
        }
        attempt(36)
    }

    func refresh() {
        // Sync talks to the twin only when this app runs it and its page is up (the engine checks the rest:
        // the TWIN- name and this twin's MAC).
        if twinMac.isEmpty { twinMac = (read(twin.p.mac) ?? "").trimmingCharacters(in: .whitespacesAndNewlines) }
        // Without the home network the twin's UDP port 4210 is forwarded from udpPort (Twin.start): its instant events
        // reach the app only as answers there (SyncEngine eventRoute).
        sync?.setTwin(address: shown && twin.ours ? twin.apiAddress : nil, mac: twinMac, udpForward: twin.ours && !twin.onLan ? twin.udpPort : nil)
        guard shown || twin.ours else { return }
        if let p = twin.process, !p.isRunning, shown { say { L("The twin is stopped", "Двойник остановлен") }; return }
        say { [twin] in
            guard let twin else { return "" }
            var s = twin.ours ? L("The twin is running", "Двойник работает")
                              : L("The twin is running (started outside the app: it stays on when you quit)",
                                  "Двойник работает (запущен не приложением — при выходе останется)")
            if twin.ours {
                if twin.onLan { s += twin.address.map { L(" · on the network: ", " · в сети: ") + $0 } ?? L(" · joining the network…", " · подключается к сети…") }
                else { s += " · \(twin.lanDecision().1)" }
            }
            return s
        }
    }

    /// The status line, kept as a function of the language so the switch retranslates it.
    func say(_ text: @escaping () -> String) { lastStatus = text; status.stringValue = text(); status.toolTip = status.stringValue }

    func showText(_ s: String) {
        let esc = s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        web.loadHTMLString("<html><body style='font:12px ui-monospace,Menlo;white-space:pre-wrap;color:#ddd;background:#111;padding:14px'>\(esc)</body></html>", baseURL: nil)
    }

    func alert(_ title: String, _ text: String) {
        let a = NSAlert(); a.messageText = title; a.informativeText = text; a.runModal()
    }

    // MARK: actions

    @objc func showPanel() { web.load(URLRequest(url: withLang(twin.pageURL))) }
    @objc func openFlasher() { NSWorkspace.shared.open(withLang(twin.flasherURL)) }
    @objc func openPortal() {
        if let u = twin.portalURL { NSWorkspace.shared.open(u); return }
        web.evaluateJavaScript("document.getElementById('portalLink') && document.getElementById('portalLink').href") { v, _ in
            if let s = v as? String, let u = URL(string: s) { NSWorkspace.shared.open(u) }
        }
    }
    @objc func restart() {
        guard twin.ours || !shown else {
            say { L("The twin was started outside the app: restart it where it was started.", "Двойник запущен не приложением: перезапустите его там, где запускали.") }
            return
        }
        say { L("Restarting…", "Перезапускаю…") }; shown = false; polls = 0
        twin.stop(); twin.start(); poll()
    }
    @objc func markHome() {
        let g = defaultGateway()
        guard !g.ip.isEmpty, !g.mac.isEmpty else {
            alert(L("No network found", "Сеть не определена"),
                  L("There is no default gateway with a known MAC. Check the connection and try again.",
                    "Нет шлюза по умолчанию с известным MAC. Проверьте подключение и повторите.")); return
        }
        guard g.interface == "en0" else {
            alert(L("Not that network", "Не та сеть"),
                  L("The default route goes through \(g.interface), and the socket_vmnet service is attached to en0.",
                    "Маршрут по умолчанию идёт через \(g.interface), а служба socket_vmnet подключена к en0.")); return
        }
        write("# \(APP_NAME): the home network - the twin joins it only here.\ninterface=\(g.interface)\ngateway_ip=\(g.ip)\ngateway_mac=\(g.mac)\n", twin.p.lan)
        alert(L("Marked as the home network", "Сеть отмечена как домашняя"),
              L("Gateway \(g.ip). The twin joins this network with an address of its own. Restarting it.",
                "Шлюз \(g.ip). Двойник будет выходить в эту сеть со своим адресом. Перезапускаю его."))
        if twin.ours { restart() }
    }
    @objc func howLan() {
        let steps = """
        cd ~/Downloads
        curl -OSL https://github.com/lima-vm/socket_vmnet/releases/download/v1.2.2/socket_vmnet-1.2.2-arm64.tar.gz
        sudo tar -C / -xzvf socket_vmnet-1.2.2-arm64.tar.gz ./opt/socket_vmnet
        sudo mkdir -p /var/log/socket_vmnet
        sudo cp /opt/socket_vmnet/share/doc/socket_vmnet/launchd/io.github.lima-vm.socket_vmnet.bridged.en0.plist /Library/LaunchDaemons/
        sudo launchctl bootstrap system /Library/LaunchDaemons/io.github.lima-vm.socket_vmnet.bridged.en0.plist
        """
        alert(L("How to enable the home network", "Как включить домашнюю сеть"),
              L("The twin gets an address of its own on the network through the socket_vmnet service (lima-vm, v1.2.2). It is installed once, with an administrator password, in Terminal:",
                "Свой адрес в сети двойнику даёт служба socket_vmnet (lima-vm, v1.2.2). Её ставят один раз, с паролем администратора, в Терминале:")
              + "\n\n" + steps + "\n\n"
              + L("Then, at home: menu \"This is my home network\". Without the service the twin works, but without an address of its own: its portal is at http://127.0.0.1:\(twin.httpPort)/.",
                  "Затем дома: меню «Это моя домашняя сеть». Без службы двойник работает, но без своего адреса: портал — на http://127.0.0.1:\(twin.httpPort)/."))
    }
    @objc func forDevelopers() {
        let api = twin.apiAddress ?? L("<the twin's address>", "<адрес двойника>")
        let name = (read(twin.p.named) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let byName = twin.onLan && name.hasPrefix("TWIN-") ? L("\(name) (by its name on the network) or ", "\(name) (по имени в сети) или ") : ""
        let sp = twin.serialPort
        alert(L("For developers", "Для разработчиков"),
              L("The twin is a device like the panel: the project's SDK (github.com/NickoScope/AnimatedPixelClock, tools/agent) works with it the same way.",
                "Двойник — такое же устройство, как панель: SDK проекта (github.com/NickoScope/AnimatedPixelClock, папка tools/agent) работает с ним так же.")
              + "\n\n" + L("The address for the SDK: ", "Адрес для SDK: ") + byName + api + "\n"
              + "  python3 tools/agent/update.py --panel \(api)   — " + L("updates from GitHub", "обновления с GitHub") + "\n"
              + "  python3 tools/agent/gallery.py show <effect> --mac <MAC>   — " + L("gallery effects", "эффекты из галереи") + "\n"
              + "  tools/agent/mcp_server.py   — " + L("the panel's MCP server, at this address", "MCP-сервер панели, по этому адресу") + "\n\n"
              + L("The twin's USB port (the board's USB-C) for esptool, PlatformIO and a serial monitor:",
                  "USB-порт двойника (как USB-C платы) для esptool, PlatformIO и монитора порта:") + "\n"
              + "  rfc2217://127.0.0.1:\(sp)\n  esptool --port rfc2217://127.0.0.1:\(sp) chip-id\n"
              + "  pio run -t upload --upload-port rfc2217://127.0.0.1:\(sp)\n  pio device monitor -p rfc2217://127.0.0.1:\(sp)\n\n"
              + L("The twin's web flasher: the Flasher button (in Chrome).", "Веб-прошивальщик двойника: кнопка «Прошивальщик» (в Chrome)."))
    }

    // MARK: updates from GitHub - tools/agent/update.py, ported: the fork's releases, the board's OTA
    // image checked against the release's SHA256SUMS.txt and against the ESP32-S3 app header, then
    // POST /update as the portal's Firmware card does; the outcome is read off the twin.
    @objc func updateFromGitHub() {
        guard let addr = twin.apiAddress else {
            alert(L("Update", "Обновление"), L("The twin was started outside the app. Update it the way it was started: tools/agent/update.py.",
                                              "Двойник запущен не приложением. Обновите его так же, как запускали: tools/agent/update.py.")); return
        }
        say { L("Checking the releases on GitHub…", "Проверяю выпуски на GitHub…") }
        // Every window here is opened from the run loop (Controller.later), never from a block on the main
        // queue: a window left open must not hold that queue - the sync switch followed from defaults, SIGTERM
        // and the status line are served while it is open.
        DispatchQueue.global().async { [self] in
            do {
                let info = try JSONSerialization.jsonObject(with: try fetch("http://\(addr)/api/info", limit: 1 << 20)) as? [String: Any] ?? [:]
                let have = info["version"] as? String ?? "?"
                guard let rel = try GitHub.latest() else { throw Failure(L("no release on GitHub carries an image for this board", "на GitHub нет выпуска с образом для этой платы")) }
                if !GitHub.ver(have).lexicographicallyPrecedes(GitHub.ver(rel.tag)) {
                    Controller.later { self.alert(L("Update", "Обновление"), L("The twin runs \(have), the latest release (\(rel.tag)).", "На двойнике \(have) — это последний выпуск (\(rel.tag)).")) }
                    return
                }
                let to = String(rel.tag.dropFirst())
                Controller.later {
                    let a = NSAlert(); a.messageText = L("Update the twin from \(have) to \(to)?", "Обновить двойника с \(have) до \(to)?")
                    a.informativeText = String(rel.notes.prefix(900)) + "\n\n" + L("The twin restarts; the new firmware confirms itself within a minute, or it is rolled back.",
                                                                                  "Двойник перезагрузится; новая прошивка подтверждает себя через минуту, иначе откатится.")
                    a.addButton(withTitle: L("Update", "Обновить")); a.addButton(withTitle: L("Cancel", "Отмена"))
                    guard a.runModal() == .alertFirstButtonReturn else { self.refresh(); return }
                    DispatchQueue.global().async { self.installRelease(rel, to: to, have: have, addr: addr) }
                }
            } catch {
                Controller.later { self.alert(L("The update failed", "Обновление не удалось"), "\(error)"); self.refresh() }
            }
        }
    }

    /// The release's image to the twin, then its confirmation read off the twin (off the main thread).
    func installRelease(_ rel: GitHub.Release, to: String, have: String, addr: String) {
        do {
            let asset = rel.assetName
            DispatchQueue.main.async { self.say { L("Downloading \(asset)…", "Скачиваю \(asset)…") } }
            let image = try GitHub.image(rel)
            DispatchQueue.main.async { self.say { L("Sending the image to the twin…", "Отправляю образ двойнику…") } }
            try GitHub.upload(image, to: addr)
            DispatchQueue.main.async { self.say { L("The twin restarts on \(to)…", "Двойник перезагружается на \(to)…") } }
            for _ in 0..<60 {                          // up to 3 minutes: reboot, Wi-Fi, 60 s of health
                Thread.sleep(forTimeInterval: 3)
                let now = DispatchQueue.main.sync { twin.apiAddress } ?? addr
                guard let d = try? fetch("http://\(now)/api/info", limit: 1 << 20),
                      let i = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { continue }
                let v = i["version"] as? String ?? "", ota = i["ota"] as? [String: Any] ?? [:]
                if v == to && (ota["state"] as? String) == "valid" {
                    Controller.later { self.alert(L("Updated", "Обновлено"), L("The twin runs \(v): the firmware has confirmed itself.", "На двойнике \(v): прошивка подтвердила себя.")); self.refresh() }
                    return
                }
                if !v.isEmpty && v == have && (i["uptime"] as? Int ?? 999) < 120 {
                    throw Failure(L("the twin came back on \(have): the new firmware did not confirm itself and was rolled back",
                                    "двойник вернулся на \(have): новая прошивка не подтвердила себя, откат"))
                }
            }
            throw Failure(L("the twin did not confirm \(to) within three minutes; see the twin's log", "за три минуты двойник не подтвердил \(to); смотрите журнал двойника"))
        } catch {
            Controller.later { self.alert(L("The update failed", "Обновление не удалось"), "\(error)"); self.refresh() }
        }
    }
    static func later(_ f: @escaping () -> Void) {
        RunLoop.main.perform(inModes: [.default], block: f)
        CFRunLoopWakeUp(CFRunLoopGetMain())
    }

    // MARK: Sync with panel (SyncEngine.swift)

    @objc func syncSwitched() { enableSync(syncToggle.state == .on) }
    @objc func toggleSync() { enableSync(!UserDefaults.standard.bool(forKey: "syncEnabled")) }
    @objc func syncNow() { sync.syncNow() }

    /// The person's switch, remembered. Off by default: turning it on is always somebody's act.
    func enableSync(_ on: Bool) {
        UserDefaults.standard.set(on, forKey: "syncEnabled")
        syncToggle.state = on ? .on : .off
        if on && (UserDefaults.standard.string(forKey: "panelAddress") ?? "").isEmpty { finder.start() }
        sync.setEnabled(on)
        // Switched off from a terminal while a sync question is on screen: it is answered for nothing now,
        // so it is closed - as Cancel, or Not now - rather than left there to be clicked later.
        if !on, let q = syncQuestion, NSApp.modalWindow == q.window { NSApp.abortModal() }
        buildMenu(); showSyncStatus()
    }

    /// A sync question's window, run as the question on screen (syncQuestion).
    func runQuestion(_ a: NSAlert) -> NSApplication.ModalResponse {
        syncQuestion = a; defer { syncQuestion = nil }
        return a.runModal()
    }

    func showSyncStatus() {
        let s = sync.status
        let f = DateFormatter(); f.dateFormat = "HH:mm:ss"
        var t = L("Sync: ", "Синхронизация: ") + s.phase.text(LANG)
        if sync.enabled && !s.peer.isEmpty { t += L(" · with ", " · с ") + s.peer }
        if let (d, m) = s.last { t += " · " + f.string(from: d) + " " + m.text(LANG) }
        if let (d, m) = s.problem { t += " · " + L("attention", "внимание") + " " + f.string(from: d) + ": " + m.text(LANG) }
        syncStatus.stringValue = t; syncStatus.toolTip = t
    }

    func list(_ x: [String]) -> String { x.isEmpty ? "—" : x.prefix(12).joined(separator: ", ") + (x.count > 12 ? " … (\(x.count))" : "") }

    /// Cancel (or Not now) is the window's default button - Return - and the others have no key: these
    /// windows come by themselves, maybe while someone is typing on the twin's page, and a Return must
    /// never write anything. The first button added is the default one, on the right.
    func safeButtons(_ a: NSAlert, _ safe: String, _ others: [String]) -> [NSButton] {
        let first = a.addButton(withTitle: safe); first.keyEquivalent = "\r"
        let rest = others.map { let b = a.addButton(withTitle: $0); b.keyEquivalent = ""; return b }
        // The keyboard focus too: NSAlert puts it on the leftmost button, the one that writes, and with Full
        // Keyboard Access on the space bar presses the focused button (the final review, 2026-09-30).
        a.window.initialFirstResponder = first
        return [first] + rest
    }

    /// Tests only (the engine passes it through its test gate): Return, as a person would press it by
    /// accident, once the window is up - the window's default button, AppKit's own notion of the one Return
    /// presses, whether or not this app is active (a key event goes nowhere while its window is not key);
    /// or a click on BUTTON.
    func pressForTesting(_ a: NSAlert, click button: NSButton? = nil) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak a] in
            guard let a else { return }
            let b = button ?? a.window.defaultButtonCell?.controlView as? NSButton
            FileHandle.standardError.write(Data("test: \(button == nil ? "Return" : "click") -> \"\(b?.title ?? "?")\" in \"\(a.messageText)\"\n".utf8))
            b?.performClick(nil)
        }
    }

    /// The first of the two windows that switch sync on (the owner, 2026-09-30 11:19). Cancel switches sync
    /// off; nothing has been written, and nothing is until the second window is answered too.
    func askConsent(_ c: SyncEngine.Consent) {
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = L("Enable sync with the panel \(c.panelName) (\(c.panelAddress), MAC \(c.panelMac))?",
                          "Включить синхронизацию с панелью \(c.panelName) (\(c.panelAddress), MAC \(c.panelMac))?")
        a.informativeText = L("From now on changes go both ways: settings, effects, screen.", "С этого момента изменения идут в обе стороны: настройки, эффекты, экран.")
            + "\n\n" + L("The twin: \(c.twin).", "Двойник: \(c.twin).")
            + "\n\n" + L("Next, the list of what differs between the panel and the twin, and which way to align them. Until you answer both, sync does nothing. Firmware goes to the panel only after a separate question. The panel's hardware settings (\(Settings.hardwareWords.en)) go from the panel to the twin only.",
                          "Дальше — список того, чем панель и двойник различаются, и вопрос, в какую сторону их выровнять. Пока вы не ответите на оба, синхронизация ничего не делает. Прошивка на панель — только после отдельного вопроса. Настройки железа панели (\(Settings.hardwareWords.ru)) идут только с панели на двойника.")
        let b = safeButtons(a, L("Cancel", "Отмена"), [L("Enable", "Включить")])
        if c.pressForTesting == 1 { pressForTesting(a) } else if c.pressForTesting == 2 { pressForTesting(a, click: b[1]) }
        switch runQuestion(a) {
        case .alertSecondButtonReturn: sync.answerConsent(id: c.id, true)
        case .abort: sync.answerConsent(id: c.id, nil)                   // closed by the app (quitting, or sync switched off)
        default: sync.answerConsent(id: c.id, nil); enableSync(false)
        }
    }

    /// The second window: the first sync of a pair, or after a massive change - which side is taken as it is.
    func askDirection(_ m: SyncEngine.Summary) {
        let a = NSAlert()
        a.messageText = L("Sync with the panel: which way?", "Синхронизация с панелью: в какую сторону?")
        var t = m.why.map { $0.text(LANG) + "\n\n" } ?? ""
        t += L("The panel: \(m.panel), firmware \(m.panelFirmware), showing \(m.panelScreen).\nThe twin: \(m.twin), firmware \(m.twinFirmware), showing \(m.twinScreen).",
               "Панель: \(m.panel), прошивка \(m.panelFirmware), на экране \(m.panelScreen).\nДвойник: \(m.twin), прошивка \(m.twinFirmware), на экране \(m.twinScreen).")
        t += "\n\n" + L("Settings that differ (\(m.settings.count)): ", "Различаются настройки (\(m.settings.count)): ") + list(m.settings)
        if !m.hardware.isEmpty {
            t += "\n" + L("The panel's hardware that differs (\(Settings.hardwareWords.en)) - it goes from the panel to the twin only: ",
                           "Различаются настройки железа панели (\(Settings.hardwareWords.ru)) — они идут только с панели на двойника: ") + list(m.hardware)
        }
        if m.effectsKnown {
            t += "\n" + L("Effects only on the panel: ", "Эффекты только на панели: ") + list(m.onlyPanel)
            t += "\n" + L("Effects only on the twin: ", "Эффекты только на двойнике: ") + list(m.onlyTwin)
            t += "\n" + L("Effects that differ: ", "Эффекты с разным содержимым: ") + list(m.differ)
            if m.bySize { t += "\n" + L("(effects compared by size: a firmware without /api/lua/source)", "(эффекты сравниваются по размеру: в прошивке нет /api/lua/source)") }
            if !m.notCarried.isEmpty {
                t += "\n" + L("Not carried - the other side refuses them (sent again when they change): ", "Не переносится — другая сторона не принимает (отправлю снова, когда эффект изменится): ")
                    + list(m.notCarried.map { $0.text(LANG) })
            }
            if !m.renames.isEmpty {
                t += "\n" + L("The twin's effect files named as the panel's but for case - they take the panel's names: ",
                              "Файлы эффектов двойника отличаются от панели только регистром — получат имена как на панели: ") + list(m.renames)
            }
        } else {
            t += "\n" + L("Effects: one side's cannot be read now. They are aligned in the direction you choose once both sides' can be; if that would remove more than \(SyncEngine.MASS_DELETES), you are asked again.",
                           "Эффекты: у одной из сторон сейчас не прочитать. Выровняю их в выбранную сторону, когда прочитаются на обеих; если придётся удалить больше \(SyncEngine.MASS_DELETES), спрошу снова.")
        }
        t += "\n\n" + L("The side you pick is taken as it is: the other one gets its settings, effects (the extra ones are removed) and screen. Firmware goes to the twin by itself; to the panel only after a separate question. Later changes go both ways. If anything listed here changes before you answer, you are asked again. Cancel: sync stays off, nothing is written.",
                         "Выбранная сторона берётся как есть: другая получает её настройки, эффекты (лишние удаляются) и экран. Прошивка на двойника уходит сама, на панель — только после отдельного вопроса. Дальнейшие изменения переносятся в обе стороны. Если до ответа изменится что-то из перечисленного, вопрос прозвучит снова. Отмена: синхронизация остаётся выключенной, ничего не записано.")
        a.informativeText = t
        _ = safeButtons(a, L("Cancel", "Отмена"), [L("From the panel to the twin", "С панели на двойника"), L("From the twin to the panel", "С двойника на панель")])
        if m.pressReturnForTesting { pressForTesting(a) }
        switch runQuestion(a) {
        case .alertSecondButtonReturn: sync.answerDirection(id: m.id, .fromPanel)
        case .alertThirdButtonReturn: sync.answerDirection(id: m.id, .fromTwin)
        case .abort: sync.answerDirection(id: m.id, nil)                 // closed by the app (quitting, or sync switched off)
        default: sync.answerDirection(id: m.id, nil); enableSync(false)
        }
    }

    /// The second window when sync is switched on again with a panel it knows: what each side changed while
    /// it was off, and which way.
    func askResume(_ m: SyncEngine.ResumeSummary) {
        let a = NSAlert()
        a.messageText = L("Sync with the panel: which way?", "Синхронизация с панелью: в какую сторону?")
        var t = L("The panel: \(m.panel).\nThe twin: \(m.twin).", "Панель: \(m.panel).\nДвойник: \(m.twin).")
        t += "\n\n" + L("While sync was off -", "Пока синхронизация была выключена —")
        t += "\n" + L("changed on the twin - settings: ", "изменено на двойнике — настройки: ") + list(m.settings)
        t += "; " + L("effects: ", "эффекты: ") + list(m.effects.map { $0.text(LANG) })
        t += "\n" + L("changed on the panel - settings: ", "изменено на панели — настройки: ") + list(m.panelSettings)
        t += "; " + L("effects: ", "эффекты: ") + list(m.panelEffects.map { $0.text(LANG) })
        if !m.conflicts.isEmpty { t += "\n" + L("Changed on both sides - the panel's is kept: ", "Изменено на обеих сторонах — останется как на панели: ") + list(m.conflicts) }
        if !m.hardware.isEmpty { t += "\n" + L("The panel's hardware changed on the twin (not carried to the panel): ", "Железо панели, изменённое на двойнике (на панель не переносится): ") + list(m.hardware) }
        if !m.notCarried.isEmpty {
            t += "\n" + L("Not carried - the other side refuses them (sent again when they change): ", "Не переносится — другая сторона не принимает (отправлю снова, когда эффект изменится): ")
                + list(m.notCarried.map { $0.text(LANG) })
        }
        t += "\n\n" + L("Return the twin to the panel: the twin becomes what the panel is; its own changes above are undone, the panel is not touched. Carry the twin's changes to the panel: the twin's changes above are written to the panel, and the panel's come to the twin. Cancel: sync stays off, nothing is written.",
                         "Вернуть двойнику состояние панели: двойник станет таким, как панель; его изменения выше пропадут, панель не трогаю. Перенести изменения двойника на панель: изменения двойника выше запишутся на панель, а изменения панели придут на двойника. Отмена: синхронизация остаётся выключенной, ничего не записано.")
        a.informativeText = t
        _ = safeButtons(a, L("Cancel", "Отмена"), [L("Return the twin to the panel", "Вернуть двойнику состояние панели"), L("Carry the twin's changes to the panel", "Перенести изменения двойника на панель")])
        if m.pressReturnForTesting { pressForTesting(a) }
        switch runQuestion(a) {
        case .alertSecondButtonReturn: sync.answerResume(id: m.id, .fromPanel)
        case .alertThirdButtonReturn: sync.answerResume(id: m.id, .toPanel)
        case .abort: sync.answerResume(id: m.id, nil)                    // closed by the app (quitting, or sync switched off)
        default: sync.answerResume(id: m.id, nil); enableSync(false)
        }
    }

    /// The twin's firmware changed: the physical panel gets it only after two yeses (the owner, 2026-09-30
    /// 11:19) - "Update the panel too?" and "Really flash the physical panel <name> with <version>?". Not
    /// now is the default button of both (Return); the buttons that flash have no key.
    func askFirmware(_ o: SyncEngine.Offer) {
        let a = NSAlert()
        a.alertStyle = o.downgrade ? .critical : .warning
        a.messageText = L("Update the panel too?", "Обновить и панель тоже?")
        var t = L("The panel: \(o.panelName), \(o.panelAddress), MAC \(o.panelMac).", "Панель: \(o.panelName), \(o.panelAddress), MAC \(o.panelMac).")
        t += "\n" + L("On the panel now: \(o.panelVersion) (built \(o.panelBuild)).", "На панели сейчас: \(o.panelVersion) (сборка \(o.panelBuild)).")
        t += "\n" + L("On the twin: \(o.version) (built \(o.build)).", "На двойнике: \(o.version) (сборка \(o.build)).")
        if o.downgrade { t += "\n\n" + L("THIS IS A DOWNGRADE: the twin's version is lower than the panel's.", "ЭТО ПОНИЖЕНИЕ ВЕРСИИ: версия на двойнике ниже, чем на панели.") }
        t += "\n\n" + L("The image: ", "Образ: ") + o.source.text(LANG) + "."
        t += "\n\n" + L("The panel restarts; the new firmware confirms itself within about a minute, or the panel rolls back to the one it has now. If the panel or either firmware changes before the image goes, nothing is sent and you are asked again. A second window asks once more.",
                         "Панель перезагрузится; новая прошивка подтверждает себя примерно за минуту, иначе панель откатится на нынешнюю. Если до отправки образа сменится панель или прошивка на любой из сторон, ничего не отправлю и спрошу заново. Следующее окно спросит ещё раз.")
        a.informativeText = t
        let first = safeButtons(a, L("Not now", "Не сейчас"), [L("Update the panel…", "Обновить панель…")])
        if o.pressForTesting == 1 { pressForTesting(a) } else if o.pressForTesting == 2 { pressForTesting(a, click: first[1]) }
        guard runQuestion(a) == .alertSecondButtonReturn else { sync.answerFirmware(id: o.id, go: false); return }
        let b = NSAlert()
        b.alertStyle = .critical
        b.messageText = L("Really flash the physical panel \(o.panelName) with \(o.version)?", "Точно прошить физическую панель \(o.panelName) версией \(o.version)?")
        b.informativeText = L("This is the real panel, not the twin: \(o.panelName), \(o.panelAddress), MAC \(o.panelMac).\n\(o.panelVersion) (built \(o.panelBuild)) → \(o.version) (built \(o.build)).",
                              "Это настоящая панель, не двойник: \(o.panelName), \(o.panelAddress), MAC \(o.panelMac).\n\(o.panelVersion) (сборка \(o.panelBuild)) → \(o.version) (сборка \(o.build)).")
            + (o.downgrade ? "\n\n" + L("THIS IS A DOWNGRADE.", "ЭТО ПОНИЖЕНИЕ ВЕРСИИ.") : "")
        _ = safeButtons(b, L("Not now", "Не сейчас"), [L("Flash the panel", "Прошить панель")])
        if o.pressForTesting == 2 { pressForTesting(b) }
        sync.answerFirmware(id: o.id, go: runQuestion(b) == .alertSecondButtonReturn)
    }

    /// What sync does, how often it looks, and who wins when both sides changed one thing.
    @objc func syncHelp() {
        let every = max(15, UserDefaults.standard.integer(forKey: "syncSettingsEveryS") <= 0 ? 60 : UserDefaults.standard.integer(forKey: "syncSettingsEveryS"))
        alert(L("How sync works", "Как работает синхронизация"),
              L("While the switch is on, the twin and the panel mirror each other both ways: the screen (page, clock style, brightness, on/off), the settings, the Lua effects and the firmware. Switched off, everything stays as it is and nothing more is written - not even what was half done.",
                "Пока переключатель включён, двойник и панель повторяют друг друга в обе стороны: экран (страница, стиль часов, яркость, вкл/выкл), настройки, Lua-эффекты и прошивку. Выключен — всё остаётся как есть, и больше ничего не пишется, даже начатое.")
              + "\n\n" + L("Switching it on asks twice: \"Enable sync with the panel …?\", then the list of what differs and which way to align. Until both are answered, nothing is written. Return means Cancel in both.",
                             "Включение спрашивает дважды: «Включить синхронизацию с панелью …?», затем список различий и направление выравнивания. Пока нет ответа на оба вопроса, ничего не пишется. Return в обоих окнах означает «Отмена».")
              + "\n\n" + L("The answer is remembered for this pair while the switch stays on: the app restarted with the switch on asks nothing, and the panel's changes made meanwhile come to the twin; only if the twin changed meanwhile is the direction asked. Switching sync off forgets it.",
                             "Ответ запоминается для этой пары, пока переключатель включён: после перезапуска приложения с включённым переключателем окон нет, изменения панели за это время приходят на двойника; только если за это время менялся двойник, будет вопрос о направлении. Выключение синхронизации это забывает.")
              + "\n\n" + L("An effect the other side refuses (its checks or its trial run, e.g. too slow) is left out: said once in the log and listed as \"not carried\"; it is sent again when it changes. Effect files named alike but for case take the panel's names on the twin.",
                             "Эффект, который другая сторона не принимает (проверки или пробный прогон, например слишком медленный), пропускается: одна запись в журнале и строка «Не переносится»; снова он отправится, когда изменится. Файлы эффектов, отличающиеся только регистром, на двойнике получают имена как на панели.")
              + "\n\n" + L("How often: the screen every 3 s (5 s while the panel is under strain), and while the panel's carousel walks, once more just after each of its steps (not under strain); effects when their list changes and every 60 s, settings every \(every) s. A device that does not answer, or a round that failed: the next look in 5 s.",
                             "Как часто: экран — раз в 3 с (5 с, когда панель под нагрузкой), а пока карусель панели идёт — ещё раз сразу после каждого её шага (не под нагрузкой); эффекты — при изменении их списка и раз в 60 с, настройки — раз в \(every) с. Устройство не ответило или раунд не удался — следующий взгляд через 5 с.")
              + "\n\n" + L("With firmware 2.7.13 on a side, it tells of each change and gesture at once (UDP): a person's page, style or brightness is carried within a fraction of a second, so are the panel's carousel steps; the polling stays under it. Inside a page: an effect's clicks (both end on the same count), the world clock's and the flight board's stop (in, turns, out), the rail board's while both show the same list. Not the media player (each gesture would reach Home Assistant twice), the yachts or the markets' inner steps.",
                             "С прошивкой 2.7.13 сторона сразу сообщает о каждом изменении и жесте (UDP): страница, стиль или яркость, выбранные человеком, переносятся за доли секунды, шаги карусели панели тоже; опрос остаётся как запасной путь. Внутри страницы: нажатия эффекта (на обеих одно и то же число), вход, повороты и выход в мировых часах и табло рейсов, в табло поездов — если на обеих один список. Не переносятся медиа (каждое действие ушло бы в Home Assistant дважды), яхты и внутренний шаг рынков.")
              + "\n\n" + L("Changes at the same time: the devices keep no time of a change, so two changes of one thing within one of these periods - up to \(every) s for a setting - count as simultaneous, and the panel's is kept (the log says \"conflict: the panel's taken - <key>\"). Only the page has its own clock: the one changed later wins.",
                             "Одновременные изменения: устройства не хранят время изменения, поэтому два изменения одного и того же в пределах одного такого периода — до \(every) с для настройки — считаются одновременными, и остаётся значение панели (в журнале: «конфликт: взята панель — <ключ>»). Только у страницы есть свои часы: побеждает изменённая позже.")
              + "\n\n" + L("The carousel is one for both: switched on or off, its settings and the pages it visits, on either side, are the same on the other a moment later. While it is on, the panel leads and the twin shows the same screens in step, within about a second; nothing of the twin's walk is written to the panel. What a person chooses (the knob, the remote, the portal) is carried as before, and both carousels wait the idle time. A restart shows its reset screen, which is nobody's choice and is not carried.",
                             "Карусель одна на двоих: включили или выключили, её настройки и страницы обхода — на любой стороне, через мгновение то же на другой. Пока она включена, ведёт панель, а двойник показывает те же экраны синхронно, с отставанием около секунды; ничего из обхода двойника на панель не пишется. То, что выбрал человек (ручка, пульт, портал), переносится как раньше, и обе карусели ждут время простоя. Перезагрузка показывает сброшенный экран — это не выбор человека, он не переносится.")
              + "\n\n" + L("Flights and trains: on the twin as on the panel. Each device has its own keys (the portal's Keys page: AeroAPI, RTT, AIS); sync never reads or copies them. Without a key the twin shows \"no key\" and makes no paid call; sync also switches off its asking Home Assistant for a flight board (fbAskHa, firmware 2.7.13), which HA would fetch with the owner's key - and it stays off.",
                             "Самолёты и поезда: на двойнике как на панели. Ключи у каждого устройства свои (вкладка Keys портала: AeroAPI, RTT, AIS); синхронизация их не читает и не копирует. Без ключа двойник показывает «нет ключа» и платных запросов не делает; ещё синхронизация выключает у него запрос табло рейсов у Home Assistant (fbAskHa, прошивка 2.7.13), за который HA платил бы ключом владельца, — и он остаётся выключенным.")
              + "\n\n" + L("Only from the panel to the twin: the panel's hardware - the microphones (source, gain, gate, AGC), the knob (direction, debounce, lockout, detent), the presence radar's setup (range, mirror, source), the climate sensor (on/off, temperature and humidity offsets, humidity correction) and the remote's receiver (on/off).",
                             "Только с панели на двойника: железо панели — микрофон (источник, усиление, порог, АРУ), ручка (направление, антидребезг, блокировка, щелчки), установка радара присутствия (дальность, зеркало, источник), датчик климата (вкл/выкл, поправки температуры и влажности, пересчёт влажности) и приёмник пульта (вкл/выкл).")
              + "\n\n" + L("Firmware: to the twin by itself; to the panel only after two yeses - \"Update the panel too?\" and \"Really flash the physical panel …?\" (Return in both means Not now). More than \(SyncEngine.MASS_SETTINGS) settings or \(SyncEngine.MASS_DELETES) effect removals at once on either side stop sync until you say which way.",
                             "Прошивка: на двойника — сама; на панель — только после двух «да»: «Обновить и панель тоже?» и «Точно прошить физическую панель …?» (Return в обоих означает «Не сейчас»). Больше \(SyncEngine.MASS_SETTINGS) настроек или \(SyncEngine.MASS_DELETES) удалений эффектов разом на любой стороне останавливают синхронизацию до вашего ответа, в какую сторону."))
    }

    /// Which panel: one found on the network (followed by its MAC), or an address typed in.
    @objc func choosePanel() {
        finder.start()
        let d = UserDefaults.standard
        let found = finder.found.values.sorted { $0.name < $1.name }
        let a = NSAlert()
        a.messageText = L("Which panel", "Какая панель")
        a.informativeText = L("The panel the twin is synced with. Twins (TWIN-…, MAC 02:54:57:49:…) are not panels. Panels found so far: \(found.count).",
                              "Панель, с которой синхронизируется двойник. Двойники (TWIN-…, MAC 02:54:57:49:…) панелями не считаются. Найдено панелей: \(found.count).")
        let popup = NSPopUpButton(frame: NSRect(x: 0, y: 32, width: 440, height: 26), pullsDown: false)
        popup.addItem(withTitle: L("Found on the network by itself (mDNS)", "Найти в сети автоматически (mDNS)"))
        for f in found {
            let twinMark = twinLike(name: f.name, mac: f.mac) ? L(" (a twin)", " (двойник)") : ""
            popup.addItem(withTitle: "\(f.name) — \(f.address) — \(f.mac) — \(f.version)\(twinMark)")
        }
        if let i = found.firstIndex(where: { $0.mac == normMac(d.string(forKey: "panelMac")) }) { popup.selectItem(at: i + 1) }
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 440, height: 24))
        field.placeholderString = L("or its address, e.g. 192.168.1.50", "или её адрес, например 192.168.1.50")
        field.stringValue = d.string(forKey: "panelAddress") ?? ""
        let v = NSView(frame: NSRect(x: 0, y: 0, width: 440, height: 60)); v.addSubview(popup); v.addSubview(field)
        a.accessoryView = v
        a.addButton(withTitle: "OK"); a.addButton(withTitle: L("Cancel", "Отмена"))
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let addr = field.stringValue.trimmingCharacters(in: .whitespaces)
        if !addr.isEmpty {
            guard validAddress(addr) else {
                alert(L("Not an address", "Это не адрес"), L("A host name or an IP address, with a port if needed: 192.168.1.50 or panel.local:80.",
                                                              "Имя или IP-адрес, при необходимости с портом: 192.168.1.50 или panel.local:80.")); return
            }
            d.set(addr, forKey: "panelAddress"); d.set("", forKey: "panelMac")
        } else {
            let i = popup.indexOfSelectedItem
            d.set("", forKey: "panelAddress"); d.set(i >= 1 && i <= found.count ? found[i - 1].mac : "", forKey: "panelMac")
        }
        sync.reconnect()
    }

    /// Other owners work in their own forks: releases and the gallery come from the repository named here.
    @objc func chooseRepo() {
        let a = NSAlert()
        a.messageText = L("Firmware releases from…", "Выпуски прошивки из…")
        a.informativeText = L("The GitHub repository (owner/name) whose releases and gallery the app uses: updates of the twin, and firmware or effects that sync fetches. The default is \(GitHub.defaultRepo); the owner of another panel names their own fork.",
                              "Репозиторий GitHub (владелец/имя), из выпусков и галереи которого приложение берёт прошивку и эффекты: обновления двойника и то, что докачивает синхронизация. По умолчанию \(GitHub.defaultRepo); владелец другой панели указывает свой форк.")
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 360, height: 24))
        field.stringValue = GitHub.repo
        a.accessoryView = field
        a.addButton(withTitle: "OK"); a.addButton(withTitle: L("Cancel", "Отмена"))
        guard a.runModal() == .alertFirstButtonReturn else { return }
        let r = field.stringValue.trimmingCharacters(in: .whitespaces)
        guard validRepo(r) else { alert(L("Not a repository", "Это не репозиторий"), L("owner/name, e.g. \(GitHub.defaultRepo)", "владелец/имя, например \(GitHub.defaultRepo)")); return }
        UserDefaults.standard.set(r, forKey: "firmwareRepo")
        sync.syncNow()
    }

    @objc func showSyncLog() {
        if !exists(sync.logFile) { write("", sync.logFile) }
        NSWorkspace.shared.open(sync.logFile)
    }

    @objc func showData() { NSWorkspace.shared.activateFileViewerSelecting([twin.p.data]) }
    @objc func showLog() { NSWorkspace.shared.open(twin.p.log) }
    @objc func about() {
        let a = NSAlert(); a.messageText = APP_NAME
        a.informativeText = L("The virtual twin of the NickoScopeMatrix LED panel: the real AnimatedPixelClock firmware on an emulated ESP32-S3 and HUB75 panel.",
                              "Виртуальный двойник LED-панели NickoScopeMatrix: настоящая прошивка AnimatedPixelClock на эмулированном ESP32-S3 и HUB75.")
            + "\n\n" + L("Firmware in the app: ", "Прошивка в приложении: ") + twin.p.firmwareVersion
            + "\n" + L("The twin's data: ", "Данные двойника: ") + twin.p.data.path
            + "\n\n" + L("Thank you to the authors whose work this is built on:", "Спасибо авторам, на чьей работе это построено:")
            + "\n• esp32sim — Joakim Eriksson (@joakimeriksson), Alice (@aliceisjustplaying), MIT"
            + "\n• AnimatedPixelClock — Keralots, MIT; " + L("the fork", "форк") + " NickoScope"
            + "\n• ESP32-HUB75-MatrixPanel-DMA — mrcodetastic"
            + "\n• Espressif — " + L("the ESP32-S3 ROM, ESP-IDF, arduino-esp32, esptool, esptool-js", "ПЗУ ESP32-S3, ESP-IDF, arduino-esp32, esptool, esptool-js")
            + "\n• ESP Web Tools — ESPHome; Improv Wi-Fi; Improv WiFi Library — jnthas"
            + "\n• socket_vmnet — " + L("the Lima project", "проект Lima")
            + "\n• " + L("the firmware's libraries: WiFiManager, ArduinoJson, Adafruit GFX, PubSubClient, arduinoWebSockets, IRremoteESP8266, QRCode, Lua",
                          "библиотеки прошивки: WiFiManager, ArduinoJson, Adafruit GFX, PubSubClient, arduinoWebSockets, IRremoteESP8266, QRCode, Lua")
        a.addButton(withTitle: "OK"); a.addButton(withTitle: L("Licenses…", "Лицензии…"))
        if a.runModal() == .alertSecondButtonReturn, let n = Bundle.main.url(forResource: "NOTICE", withExtension: "md") {
            NSWorkspace.shared.open(n)
        }
    }

    // Links that leave the page open in the default browser.
    func webView(_ w: WKWebView, createWebViewWith c: WKWebViewConfiguration, for a: WKNavigationAction, windowFeatures f: WKWindowFeatures) -> WKWebView? {
        if let u = a.request.url { NSWorkspace.shared.open(u) }
        return nil
    }
    func webView(_ w: WKWebView, decidePolicyFor a: WKNavigationAction, decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if a.navigationType == .linkActivated, let u = a.request.url, u.host != "127.0.0.1" || u.port != twin.port {
            NSWorkspace.shared.open(u); decisionHandler(.cancel); return
        }
        decisionHandler(.allow)
    }

    func windowWillClose(_ n: Notification) { NSApp.terminate(nil) }
    func applicationShouldTerminateAfterLastWindowClosed(_ s: NSApplication) -> Bool { true }
    /// Quitting stops sync's writes as a switch-off would, but it is not one: the switch stays on, and so does the
    /// consent kept for the pair - the next start resumes without the two windows.
    func applicationWillTerminate(_ n: Notification) { sync?.quit(); twin.stop() }
}

let app = NSApplication.shared
let controller = Controller()
app.delegate = controller
// A test build (another bundle identifier) stays out of the Dock and the app switcher, so the owner never
// mistakes it for his twin and quits the wrong one; build.sh also marks it LSUIElement.
app.setActivationPolicy(Bundle.main.bundleIdentifier == STANDARD_BUNDLE_ID ? .regular : .accessory)
// SIGTERM (kill, a logout) quits as the menu does, so the twin's engine is stopped, not orphaned. An open
// question is closed first - AppKit does not quit under a modal window - and a closed question is never a
// yes: "Update the panel too?" answers Not now, the others Cancel.
signal(SIGTERM, SIG_IGN)
let termSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
termSource.setEventHandler {
    if NSApp.modalWindow != nil { NSApp.abortModal(); DispatchQueue.main.async { NSApp.terminate(nil) } }
    else { NSApp.terminate(nil) }
}
termSource.resume()
app.run()
