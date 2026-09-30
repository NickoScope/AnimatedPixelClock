// TWIN — панель: the virtual twin as a Mac app.
//
// Opening the app starts the twin (tools/twin/twin.py run --lan --web PORT) and shows its panel page
// in the window; quitting the app, or closing its window, stops the twin it started (the owner,
// 2026-09-29: the twin runs only while it is worked with). A twin that is already running - started
// from a terminal - is shown as it is and left running on quit. The engine, twin.py and the page are
// unchanged: this is a window and two buttons around them.
//
// Built by build.sh (swiftc, no Xcode project); the path of twin.py is written into Info.plist
// (TwinScript) at build time. Settings (defaults write <bundle id> KEY VALUE, or -KEY VALUE on the
// command line): port (8790), lan (YES).

import AppKit
import WebKit

final class Twin {
    let script: String
    let port: Int
    let lan: Bool
    private(set) var process: Process?
    private(set) var log = ""
    var onChange: (() -> Void)?

    init(script: String, port: Int, lan: Bool) { self.script = script; self.port = port; self.lan = lan }

    var pageURL: URL { URL(string: "http://127.0.0.1:\(port)/panel.html")! }
    var flasherURL: URL { URL(string: "http://127.0.0.1:\(port)/flasher/")! }
    var ours: Bool { process?.isRunning == true }

    /// The engine's web server answers: a twin runs, ours or one started elsewhere.
    func answers(_ done: @escaping (Bool) -> Void) {
        var req = URLRequest(url: pageURL); req.timeoutInterval = 1.5
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            done((resp as? HTTPURLResponse)?.statusCode == 200)
        }.resume()
    }

    func start() {
        guard process?.isRunning != true else { return }
        log = ""
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
        p.arguments = [script, "run", "--web", String(port)] + (lan ? ["--lan"] : [])
        // twin.py execs the engine, so this process is the engine itself once it has started.
        p.currentDirectoryURL = URL(fileURLWithPath: (script as NSString).deletingLastPathComponent)
        let pipe = Pipe()
        p.standardOutput = pipe; p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
            DispatchQueue.main.async {
                guard let self else { return }
                self.log += s
                if self.log.count > 200_000 { self.log = String(self.log.suffix(100_000)) }
                self.onChange?()
            }
        }
        p.terminationHandler = { [weak self] _ in DispatchQueue.main.async { self?.onChange?() } }
        do { try p.run(); process = p } catch { log = "twin.py не запустился: \(error.localizedDescription)\n" }
        onChange?()
    }

    /// Stop the twin this app started: SIGTERM, as twin.py's own users do, then wait for it.
    func stop(wait: Bool = true) {
        guard let p = process, p.isRunning else { return }
        p.terminate()
        if wait {
            let deadline = Date().addingTimeInterval(10)
            while p.isRunning && Date() < deadline { usleep(100_000) }
            if p.isRunning { kill(p.processIdentifier, SIGKILL) }
        }
    }

    /// The twin's address on the home network, from its console ("IP Address: ..."), if it said so.
    var address: String? {
        guard let r = log.range(of: "IP Address: ", options: .backwards) else { return nil }
        return String(log[r.upperBound...].prefix { "0123456789.".contains($0) })
    }
}

final class Controller: NSObject, NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate {
    var window: NSWindow!
    var web: WKWebView!
    let status = NSTextField(labelWithString: "")
    var twin: Twin!
    var shown = false
    var polls = 0

    func applicationDidFinishLaunching(_ note: Notification) {
        let info = Bundle.main.infoDictionary ?? [:]
        let d = UserDefaults.standard
        d.register(defaults: ["port": 8790, "lan": true])
        twin = Twin(script: info["TwinScript"] as? String ?? "", port: d.integer(forKey: "port"), lan: d.bool(forKey: "lan"))
        twin.onChange = { [weak self] in self?.refresh() }
        buildMenu(); buildWindow()
        twin.answers { [weak self] up in DispatchQueue.main.async {
            guard let self else { return }
            if !up {
                guard FileManager.default.fileExists(atPath: self.twin.script) else {
                    self.say("Не найден twin.py: \(self.twin.script). Соберите приложение заново (tools/twin/app/build.sh).")
                    return
                }
                self.twin.start()
            }
            self.poll()
        } }
    }

    func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 820),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        window.title = "TWIN — панель"
        window.setFrameAutosaveName("TwinPanelWindow")
        window.delegate = self
        let cfg = WKWebViewConfiguration()
        web = WKWebView(frame: .zero, configuration: cfg)
        web.navigationDelegate = self; web.uiDelegate = self
        web.setValue(false, forKey: "drawsBackground")

        func button(_ title: String, _ action: Selector) -> NSButton {
            let b = NSButton(title: title, target: self, action: action); b.bezelStyle = .rounded; return b
        }
        let bar = NSStackView(views: [button("Панель", #selector(showPanel)), button("Портал ↗", #selector(openPortal)),
                                      button("Прошивальщик ↗", #selector(openFlasher)), button("Перезапустить", #selector(restart)),
                                      status])
        bar.orientation = .horizontal; bar.spacing = 8
        bar.edgeInsets = NSEdgeInsets(top: 6, left: 10, bottom: 6, right: 10)
        status.lineBreakMode = .byTruncatingTail
        status.textColor = .secondaryLabelColor
        let root = NSStackView(views: [bar, web])
        root.orientation = .vertical; root.spacing = 0; root.alignment = .leading
        web.translatesAutoresizingMaskIntoConstraints = false
        root.addConstraints([web.widthAnchor.constraint(equalTo: root.widthAnchor)])
        window.contentView = root
        window.center(); window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        say("Запускаю двойника…")
    }

    func buildMenu() {
        let main = NSMenu()
        let app = NSMenuItem(); main.addItem(app)
        let m = NSMenu()
        m.addItem(withTitle: "Перезапустить двойника", action: #selector(restart), keyEquivalent: "r")
        m.addItem(.separator())
        m.addItem(withTitle: "Закрыть и остановить двойника", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        app.submenu = m
        let edit = NSMenuItem(); main.addItem(edit)
        let e = NSMenu(title: "Правка")
        e.addItem(withTitle: "Копировать", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        e.addItem(withTitle: "Вставить", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        e.addItem(withTitle: "Выделить всё", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        edit.submenu = e
        NSApp.mainMenu = main
    }

    /// Wait for the engine's web server, then show the page; while waiting, show twin.py's output.
    func poll() {
        twin.answers { [weak self] up in DispatchQueue.main.async {
            guard let self else { return }
            if up { if !self.shown { self.shown = true; self.showPanel() }; self.refresh(); return }
            self.polls += 1
            if let p = self.twin.process, !p.isRunning {
                self.say("Двойник не запустился (код \(p.terminationStatus)).")
                self.showText("twin.py завершился:\n\n" + self.twin.log)
                return
            }
            if self.polls % 4 == 0 { self.showText("Запуск двойника…\n\n" + self.twin.log) }
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.poll() }
        } }
    }

    func refresh() {
        var s = twin.ours ? "работает (запущен приложением)" : (shown ? "работает (запущен не приложением — при выходе останется)" : "")
        if let a = twin.address { s += " · в сети: \(a)" }
        if let p = twin.process, !p.isRunning, shown { s = "остановлен" }
        if !s.isEmpty { say("Двойник " + s) }
    }

    func say(_ s: String) { status.stringValue = s }

    func showText(_ s: String) {
        let esc = s.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "<", with: "&lt;")
        web.loadHTMLString("<html><body style='font:12px ui-monospace,Menlo;white-space:pre-wrap;color:#ddd;background:#111;padding:14px'>\(esc)</body></html>", baseURL: nil)
    }

    @objc func showPanel() { web.load(URLRequest(url: twin.pageURL)) }
    @objc func openFlasher() { NSWorkspace.shared.open(twin.flasherURL) }
    @objc func openPortal() {
        if let a = twin.address, let u = URL(string: "http://\(a)/") { NSWorkspace.shared.open(u); return }
        // Not ours, or it has not said its address yet: the page's own portal link knows it.
        web.evaluateJavaScript("document.getElementById('portalLink') && document.getElementById('portalLink').href") { v, _ in
            if let s = v as? String, let u = URL(string: s) { NSWorkspace.shared.open(u) }
        }
    }
    @objc func restart() {
        guard twin.ours else { say("Двойник запущен не приложением: перезапустите его там, где запускали."); return }
        say("Перезапускаю…"); shown = false; polls = 0
        twin.stop()
        twin.start(); poll()
    }

    // Links that leave the page (the portal, target=_blank) open in the default browser.
    func webView(_ w: WKWebView, createWebViewWith c: WKWebViewConfiguration, for a: WKNavigationAction,
                 windowFeatures f: WKWindowFeatures) -> WKWebView? {
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
    func applicationWillTerminate(_ n: Notification) { twin.stop() }
}

let app = NSApplication.shared
let controller = Controller()
app.delegate = controller
app.setActivationPolicy(.regular)
app.run()
