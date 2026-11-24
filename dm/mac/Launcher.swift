// Mac app launcher: the settings window shown before the game starts, in the launcher's own process (launcher.c calls dm_mac_launcher before it starts the host). Its settings come from the
// app's Resources/dm/launcher.json (dm/games/settings.py with SETTINGS_PLATFORM=mac); the model and the rows are the iOS launcher's (dm/ios/Sources/LauncherSettings.swift, LauncherRow.swift),
// and so is the store: launcher.<VAR> in the app's preferences, which launcher.c reads when the window is not shown (docs/frontends.md, Saved choices). Basic settings (level basic) come
// first, the others under Advanced settings, each in the list's sections. Play puts the settings into this process's environment, which the host inherits; Quit and closing the window end the
// app without the game. "Show this window at launch" off: no window unless Option is held at launch, and launcher.c applies the saved choices. The window never takes focus by itself: an app
// opened in the background (open -g) keeps its window behind until the player switches to it
import AppKit
import QuartzCore
import SwiftUI

private func info(_ key: String) -> String? { Bundle.main.object(forInfoDictionaryKey: key) as? String }

/// The host's comparison of the two saves (dm/host/save_sync.c). This launcher links save_sync.o but imports no C header (pack_app.py builds it with swiftc and no bridging
/// header), so the symbol is declared here.
@_silgen_name("save_sync_report")
private func save_sync_report(_ save: UnsafePointer<CChar>, _ title: UnsafePointer<CChar>, _ folder: UnsafePointer<CChar>?, _ waitMs: Int32,
                              _ out: UnsafeMutablePointer<CChar>, _ n: Int) -> Int32

/// Cloud saves before Play: this Mac's save against the cloud's (SaveSyncReport, shared with the iPhone launcher), and the question it needs, as alerts.
/// It runs with the window's Play and, with the window off, before dmMacLauncher returns; DM_LAUNCHER=0 asks nothing (the host takes the newer save).
enum SaveCheck {
    enum Outcome { case play(String?), retry, cancel }

    /// Blocks while the cloud's file comes (SaveSyncReport.waitMs at most): call it off the main thread. nil: no report (sync is then left to the host as before)
    static func report() -> SaveSyncReport? {
        // getenv, not ProcessInfo.environment: launcher.c sets DM_SAVE and applies the env files in this process before the window
        guard let save = getenv("DM_SAVE") else { fputs("launcher: save sync: no DM_SAVE\n", stderr); return nil }
        guard let id = info("DMTitleID"), !id.isEmpty else { fputs("launcher: save sync: no DMTitleID\n", stderr); return nil }
        // the folder as launcher.c will pass it (cloud_saves_folder): DM_SAVE_SYNC from the launch environment or an env file, else the window's choice; "0" = off
        let folder = getenv("DM_SAVE_SYNC").map { String(cString: $0) } ?? (UserDefaults.standard.string(forKey: "cloudSaves.folder") ?? "")
        var buf = [CChar](repeating: 0, count: 1024)
        let r = (folder.isEmpty ? "0" : folder).withCString { save_sync_report(save, id, $0, SaveSyncReport.waitMs, &buf, buf.count) }
        let line = String(cString: buf)
        guard r == 0 else { fputs("launcher: save sync: no report on \(String(cString: save))\n", stderr); return nil }
        fputs("launcher: save sync: \(line)\n", stderr)
        return SaveSyncReport(line: line)
    }

    /// The question the report needs, as a modal alert; `cancel` names the way back ("Cancel" to the window, "Quit" without one)
    static func ask(_ r: SaveSyncReport?, cancel: String) -> Outcome {
        switch SaveSyncReport.step(r) {
        case .play(let choice): return .play(choice)
        case .choose:
            guard let r else { return .play(nil) }
            let a = NSAlert()
            a.messageText = SaveSyncReport.chooseTitle
            a.informativeText = r.chooseMessage(here: "Mac")
            a.alertStyle = .warning
            a.addButton(withTitle: r.localLabel(here: "Mac"))
            a.addButton(withTitle: r.cloudLabel)
            a.addButton(withTitle: cancel)
            a.buttons[0].keyEquivalent = ""   // no default: Return must not pick a save (the newer is not always the one to keep)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play(r.localChoice)
            case .alertSecondButtonReturn: return .play(r.cloudChoice)
            default: return .cancel
            }
        case .unreadable:
            let a = NSAlert()
            a.messageText = SaveSyncReport.unreadableTitle
            a.informativeText = r?.unreadableMessage(here: "Mac") ?? ""
            a.addButton(withTitle: "Play This Mac's Save")
            a.addButton(withTitle: "Try Again")
            a.addButton(withTitle: cancel)
            a.buttons[2].keyEquivalent = "\u{1b}"
            switch a.runModal() {
            case .alertFirstButtonReturn: return .play("offline")   // the mount does not wait for the cloud again
            case .alertSecondButtonReturn: return .retry
            default: return .cancel
            }
        }
    }

    static func apply(_ choice: String?) {
        guard let choice else { return }
        setenv("DM_SAVE_SYNC_CHOICE", choice, 1)
        fputs("launcher: DM_SAVE_SYNC_CHOICE=\(choice)\n", stderr)
    }

    /// With the window off: the check and its question before the game, the app in front only while it asks. false: the player quit
    static func withoutWindow() -> Bool {
        final class Box: @unchecked Sendable { let lock = NSLock(); var report: SaveSyncReport?; var waiting = false }
        let app = NSApplication.shared
        defer { app.setActivationPolicy(.prohibited) }
        while true {
            let box = Box(), sem = DispatchSemaphore(value: 0)
            DispatchQueue.global(qos: .userInitiated).async {
                let r = report()
                box.lock.lock(); box.report = r; box.lock.unlock()
                sem.signal()
                DispatchQueue.main.async { if box.waiting { app.abortModal() } }   // runs inside the waiting alert's modal loop, if there is one
            }
            if sem.wait(timeout: .now() + 0.5) == .timedOut {   // a cloud file to download: say so, and let the player go on without it
                app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
                let a = NSAlert()
                a.messageText = "Checking the cloud save…"
                a.informativeText = "Waiting for iCloud to deliver this game's save."
                a.addButton(withTitle: "Play Without It")
                a.addButton(withTitle: "Quit")
                box.waiting = true
                let answer = a.runModal()
                box.waiting = false
                if answer == .alertFirstButtonReturn { apply("offline"); return true }
                if answer == .alertSecondButtonReturn { return false }
                sem.wait()   // aborted: the report is in
            }
            box.lock.lock(); let r = box.report; box.lock.unlock()
            if case .play(let choice) = SaveSyncReport.step(r) { apply(choice); return true }
            app.setActivationPolicy(.regular); app.activate(ignoringOtherApps: true)
            switch ask(r, cancel: "Quit") {
            case .play(let choice): apply(choice); return true
            case .retry: continue
            case .cancel: return false
            }
        }
    }
}

private enum AscensionPage: String, CaseIterable {
    case display = "Display", graphics = "Graphics", performance = "Performance", audio = "Audio", diagnostics = "Diagnostics"

    var icon: String {
        switch self {
        case .display: return "display"
        case .graphics: return "sparkles"
        case .performance: return "speedometer"
        case .audio: return "speaker.wave.2"
        case .diagnostics: return "waveform.path.ecg"
        }
    }

    static func page(for setting: Setting) -> Self {
        switch setting.env {
        case "DM_OPMODE", "DM_FULLSCREEN", "DM_EXPERIMENTAL_HDR", "DM_SCREEN_ASPECT": return .display
        case "DM_GFX_SCALE", "DM_PRESENT_UPSCALE", "DM_PRESENT_SHARPEN", "DM_DYNAMIC_RESOLUTION", "DM_UPSCALER", "DM_UPSCALER_SHARPNESS": return .graphics
        case "DM_FRAMEGEN", "DM_FPS_TARGET", "DM_PRESENT_SNAP", "DM_PSO_MTL4", "DM_PSO_PREBUILD", "DM_NVN_ENC_THREAD", "DM_FPS_HOLD": return .performance
        default: return .diagnostics
        }
    }
}

private struct AscensionSidebar: NSViewRepresentable {
    @Binding var page: AscensionPage

    func makeCoordinator() -> Coordinator { Coordinator(page: $page) }

    func makeNSView(context: Context) -> NSVisualEffectView {
        let material = NSVisualEffectView()
        material.material = .sidebar
        material.blendingMode = .behindWindow
        let stack = NSStackView()
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 5
        stack.translatesAutoresizingMaskIntoConstraints = false
        let glass = NSGlassEffectView()
        glass.cornerRadius = 16
        glass.translatesAutoresizingMaskIntoConstraints = false
        glass.contentView = stack
        material.addSubview(glass)
        NSLayoutConstraint.activate([
            glass.leadingAnchor.constraint(equalTo: material.leadingAnchor, constant: 12),
            glass.trailingAnchor.constraint(equalTo: material.trailingAnchor, constant: -12),
            glass.topAnchor.constraint(equalTo: material.topAnchor, constant: 18),
            stack.leadingAnchor.constraint(equalTo: glass.leadingAnchor, constant: 8),
            stack.trailingAnchor.constraint(equalTo: glass.trailingAnchor, constant: -8),
            stack.topAnchor.constraint(equalTo: glass.topAnchor, constant: 8),
            stack.bottomAnchor.constraint(equalTo: glass.bottomAnchor, constant: -8)
        ])
        for (index, page) in AscensionPage.allCases.enumerated() {
            let button = NSButton(title: page.rawValue, target: context.coordinator, action: #selector(Coordinator.select(_:)))
            button.image = NSImage(systemSymbolName: page.icon, accessibilityDescription: nil)
            button.imagePosition = .imageLeading
            button.bezelStyle = .recessed
            button.setButtonType(.pushOnPushOff)
            button.alignment = .left
            button.tag = index
            stack.addArrangedSubview(button)
            button.widthAnchor.constraint(equalTo: stack.widthAnchor).isActive = true
            button.heightAnchor.constraint(greaterThanOrEqualToConstant: 30).isActive = true
            context.coordinator.buttons.append(button)
        }
        return material
    }

    func updateNSView(_ view: NSVisualEffectView, context: Context) {
        for (index, button) in context.coordinator.buttons.enumerated() {
            button.state = AscensionPage.allCases[index] == page ? .on : .off
        }
    }

    final class Coordinator: NSObject {
        var buttons: [NSButton] = []
        var page: Binding<AscensionPage>
        init(page: Binding<AscensionPage>) { self.page = page }
        @objc func select(_ sender: NSButton) {
            page.wrappedValue = AscensionPage.allCases[sender.tag]
            sender.state = .on
        }
    }
}

private func launcherAppearance(_ choice: String) -> NSAppearance? {
    switch choice {
    case "light": return NSAppearance(named: .aqua)
    case "dark": return NSAppearance(named: .darkAqua)
    default: return nil
    }
}

struct LauncherView: View {
    @ObservedObject var m: LauncherModel
    let done: (Bool) -> Void
    let fit: (CGFloat) -> Void
    @AppStorage("window.show") private var show = true
    @AppStorage("window.appearance") private var appearance = "system"
    @AppStorage("window.advanced") private var advanced = false
    @State private var checking = false
    @State private var check = 0   // which Play the running check is for: one the player went on from without it is dropped

    @State private var page = AscensionPage.display
    @AppStorage("DMSpatialAudioRouteV2") private var audioMode = 0
    @AppStorage("DMSpeakerOutputPreference") private var speakerMode = 0
    private var ascension: Bool { Bundle.main.bundleIdentifier?.hasPrefix("org.nxnative.oot.ascension.") == true }

    var body: some View {
        if ascension { ascensionBody } else { legacyBody }
    }

    private var ascensionBody: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                AscensionSidebar(page: $page).frame(width: 172)
                Divider()
                VStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: 14) {
                        Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 48, height: 48).accessibilityHidden(true)
                        VStack(alignment: .leading, spacing: 3) {
                            Text("Ascension").font(.title2.weight(.semibold))
                            Text("Ocarina of Time · Apple Silicon Edition").foregroundStyle(.secondary)
                        }
                    }.padding(22)
                    Divider()
                    Form {
                        Section {
                            if page == .graphics, let tier = m.settings.first(where: { $0.env == "DM_GFX_SCALE" }) {
                                preset(tier)
                            }
                            ForEach(m.settings.filter { AscensionPage.page(for: $0) == page && m.visible($0) && $0.env != "DM_GFX_SCALE" }) {
                                SettingRow(s: $0, m: m)
                            }
                        } header: { Text(page.rawValue).accessibilityAddTraits(.isHeader) }
                        availability
                    }.formStyle(.grouped).id(page)
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
            }.frame(maxHeight: .infinity)
            Divider()
            HStack {
                Toggle("Show this window at launch", isOn: $show).toggleStyle(.checkbox)
                Spacer()
                Button("Quit") { done(false) }
                Button("Launch Game") { done(true) }
                    .buttonStyle(.borderedProminent).controlSize(.large).keyboardShortcut(.defaultAction)
            }.padding(16)
        }
        .onChange(of: appearance) { _, choice in
            NSApp.keyWindow?.appearance = launcherAppearance(choice)
        }
    }

    private func preset(_ setting: Setting) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Graphics quality").font(.headline)
            HStack(spacing: 12) {
                ForEach(setting.options ?? [], id: \.[0]) { option in
                    let selected = m.values[setting.env] == option[0]
                    Button {
                        m.edit(setting, option[0])
                    } label: {
                        HStack {
                            Image(systemName: option[0] == "1" ? "bolt.fill" : "sparkles")
                            VStack(alignment: .leading, spacing: 3) {
                                Text(option[1]).font(.headline)
                                Text((option[0] == "1" ? "1536 × 864" : "1920 × 1080") +
                                     (m.values["DM_SCREEN_ASPECT"] == "auto" ? " · 16:9 baseline" : " docked"))
                                    .font(.caption).foregroundStyle(.secondary)
                            }
                            Spacer(minLength: 0)
                            if selected { Image(systemName: "checkmark.circle.fill").foregroundStyle(Color.accentColor) }
                        }.padding(9).frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .buttonStyle(.bordered)
                    .accessibilityAddTraits(selected ? .isSelected : [])
                    .disabled(m.fixed.contains(setting.env))
                }
            }
            Text(m.fixed.contains(setting.env) ? "Graphics quality is set by a launch override." : "Applied at launch, independently of console mode.")
                .font(.footnote).foregroundStyle(.secondary)
            if m.values["DM_SCREEN_ASPECT"] == "auto" {
                Text("Matching a taller window keeps the scene width and increases its height. At 16:10, docked Performance is 1536 × 960 and Quality is 1920 × 1200. More pixels may increase GPU time.")
                    .font(.footnote).foregroundStyle(.secondary)
            }
        }.padding(.vertical, 4)
    }

    @ViewBuilder private var availability: some View {
        switch page {
        case .display:
            let screen = NSApp.keyWindow?.screen ?? NSScreen.main
            Section("Appearance") {
                Picker("Appearance", selection: $appearance) {
                    Text("System").tag("system")
                    Text("Light").tag("light")
                    Text("Dark").tag("dark")
                }.pickerStyle(.segmented)
            }
            Section("Display status") {
                LabeledContent("Launcher display", value: screen?.localizedName ?? "Unavailable")
                if let screen {
                    let mode = screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1
                    LabeledContent("Display capability", value: mode ? "Extended dynamic range" : "Standard dynamic range")
                }
                LabeledContent("Effective output", value: "Reported by the game after launch")
                if m.settings.contains(where: { $0.env == "DM_SCREEN_ASPECT" }) {
                    Text("Match Display / Window uses the actual game window’s safe area, including native fullscreen. The effective picture shape is reported in Display and Diagnostics after launch. Resize changes take effect on the next launch.")
                        .foregroundStyle(.secondary)
                }
                Text("HDR10 requires Docked mode and a compatible game display. Otherwise the game uses SDR.").foregroundStyle(.secondary)
            }
        case .graphics:
            Section("Availability") {
                Text("MetalFX Spatial is available for SDR presentation when the GPU and image size support it. Temporal reconstruction and SMAA are unavailable.")
                    .foregroundStyle(.secondary)
            }
        case .audio:
            Section("Game audio") {
                Picker("Requested output", selection: Binding(get: { audioMode }, set: { mode in
                    if (0...2).contains(mode) {
                        speakerMode = mode
                    } else if (0...2).contains(audioMode) {
                        speakerMode = audioMode
                    }
                    audioMode = mode
                })) {
                    ForEach(Array(audioModes.enumerated()), id: \.offset) { index, label in
                        Text(label).tag(index)
                    }
                }
                Text("Change routing in the in-game Sound menu. Effective output, personalization and head tracking are reported there after playback starts.")
                    .foregroundStyle(.secondary)
                Text("Discrete 5.1 requires a verified output layout. AirPods head tracking requires a supported active route.")
                    .foregroundStyle(.secondary)
            }
        case .diagnostics:
            Section("Runtime status") {
                Text("Frame timing and audio counters become available during gameplay. The Performance HUD can be enabled above.")
                    .foregroundStyle(.secondary)
                Button("Reset to tested defaults") { m.reset() }.disabled(!m.changed)
            }
        case .performance:
            Section("Frame pacing") {
                Text("The frame-rate target is a request. Enable the Performance HUD in Diagnostics to check actual pacing.").foregroundStyle(.secondary)
            }
        }
    }

    private let audioModes = ["Automatic", "Stereo", "Prefer discrete 5.1", "Fixed binaural", "Apple SpatialMixer fixed", "AirPods head tracking"]

    private var legacyBody: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Image(nsImage: NSApp.applicationIconImage).resizable().frame(width: 64, height: 64).accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(info("CFBundleName") ?? "Game").font(.title2.weight(.semibold))
                    Text("Version \(info("CFBundleShortVersionString") ?? "1.0")").foregroundStyle(.secondary)
                }
                Spacer()
            }
            .padding([.horizontal, .top], 20)
            Form {
                settingSections(basic: true)
                if (Bundle.main.object(forInfoDictionaryKey: "DMSaveSyncChoice") as? Bool) != false {
                    CloudSavesSection()
                }
                Section {
                    DisclosureGroup("Advanced settings", isExpanded: $advanced) {}
                }
                if advanced {
                    settingSections(basic: false)
                    Section {
                        Button("Reset to tested defaults") { m.reset() }
                            .disabled(!m.changed)
                    } footer: {
                        Text("Brings back all settings this game was tested with.")
                    }
                }
            }
            .formStyle(.grouped)
            .onScrollGeometryChange(for: CGFloat.self) {
                $0.contentSize.height + $0.contentInsets.top + $0.contentInsets.bottom - $0.containerSize.height
            } action: { _, extraHeight in
                fit(extraHeight)
            }
            HStack {
                Toggle("Show this window at launch", isOn: $show).toggleStyle(.checkbox)
                Spacer()
                if checking {
                    ProgressView().controlSize(.small)
                    Text("Checking the cloud save…").foregroundStyle(.secondary)
                    Button("Play Without It") { checking = false; SaveCheck.apply("offline"); done(true) }
                }
                Button("Quit") { done(false) }
                Button("Play") { start() }.keyboardShortcut(.defaultAction).disabled(checking)
            }
            .padding(20)
        }
        .frame(minWidth: 520, idealWidth: 560)
    }

    /// Play, once the cloud's save is compared with this Mac's: a save that changed on both sides is the player's pick, not the newer file's (SaveCheck)
    private func start() {
        check += 1; checking = true
        let mine = check
        DispatchQueue.global(qos: .userInitiated).async {
            let r = SaveCheck.report()
            DispatchQueue.main.async {
                guard checking, check == mine else { return }   // the player played without it
                checking = false
                switch SaveCheck.ask(r, cancel: "Cancel") {
                case .play(let choice): SaveCheck.apply(choice); done(true)
                case .retry: start()
                case .cancel: break   // back to the window, nothing chosen
                }
            }
        }
    }

    @ViewBuilder private func settingSections(basic: Bool) -> some View {
        ForEach(m.sections(basic: basic), id: \.self) { section in
            let rows = m.rows(in: section, basic: basic)
            if !rows.isEmpty {
                Section(section) {
                    ForEach(rows) { SettingRow(s: $0, m: m) }
                }
            }
        }
    }
}

/// Cloud saves: a folder the save is synced through (dm/host/save_sync.h), the one the iPhone app picks in its launcher's Cloud saves section. It is kept in this app's
/// preferences as cloudSaves.folder, which launcher.c puts into DM_SAVE_SYNC before the host starts; a DM_SAVE_SYNC an env file or the launch environment set wins, and is shown here.
struct CloudSavesSection: View {
    @AppStorage("cloudSaves.folder") private var folder = ""
    private let fixed: String? = {
        let env = ProcessInfo.processInfo.environment
        let keys = ((env["DM_ENV_FILE_KEYS"] ?? "") + " " + (env["DM_ENV_LAUNCH_KEYS"] ?? "")).split(separator: " ")
        return keys.contains("DM_SAVE_SYNC") || env["DM_SAVE_SYNC"] != nil ? env["DM_SAVE_SYNC"] : nil
    }()

    var body: some View {
        Section {
            LabeledContent("Cloud saves") {
                Text(fixed.map { "\(Self.shown($0)) (env.txt)" } ?? (folder.isEmpty ? "Off" : Self.shown(folder)))
                    .foregroundStyle(.secondary).lineLimit(1).truncationMode(.head)
            }
            if fixed == nil {
                HStack {
                    Button(folder.isEmpty ? "Choose a folder…" : "Choose a different folder…", action: choose)
                    if !folder.isEmpty { Button("Turn off") { folder = "" } }
                }
            }
        } footer: {
            Text("Choose the iCloud Drive folder the iPhone app uses for this game. The save comes in when the game starts and goes out after each save; a save that is replaced is kept in the save's -backups folder.")
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.allowsMultipleSelection = false; panel.canCreateDirectories = true
        panel.prompt = "Use for Cloud Saves"
        panel.message = "The folder the iPhone app chose in its launcher's Cloud saves"
        panel.directoryURL = URL(fileURLWithPath: folder.isEmpty ? Self.drive : folder)
        if panel.runModal() == .OK, let url = panel.url { folder = url.path }
    }

    static let drive = NSHomeDirectory() + "/Library/Mobile Documents/com~apple~CloudDocs"   // iCloud Drive, as the Finder shows it

    /// A path under iCloud Drive as the Finder and the Files app name it: iCloud Drive › Documents › dev › Zelda; any other path as it is
    static func shown(_ path: String) -> String {
        guard path == drive || path.hasPrefix(drive + "/") else { return path }
        return (["iCloud Drive"] + path.dropFirst(drive.count).split(separator: "/").map(String.init)).joined(separator: " › ")
    }
}

private final class Launcher: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let model: LauncherModel
    var window: NSWindow?
    var play = false

    init(_ model: LauncherModel) { self.model = model }

    func applicationDidFinishLaunching(_ n: Notification) {
        let name = info("CFBundleName") ?? "Game"
        let menu = NSMenu(), item = NSMenuItem(), app = NSMenu()
        app.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        app.addItem(withTitle: "Quit \(name)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        item.submenu = app; menu.addItem(item); NSApp.mainMenu = menu
        let ascension = Bundle.main.bundleIdentifier?.hasPrefix("org.nxnative.oot.ascension.") == true
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: ascension ? 810 : 560, height: ascension ? 610 : 660), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        w.title = name; w.isReleasedWhenClosed = false; w.delegate = self
        w.contentMinSize = ascension ? NSSize(width: 680, height: 460) : NSSize(width: 520, height: 0)
        window = w
        if ascension {
            w.appearance = launcherAppearance(UserDefaults.standard.string(forKey: "window.appearance") ?? "system")
            w.center()
        }
        else if let screen = w.screen ?? NSScreen.main {
            w.setFrameTopLeftPoint(NSPoint(x: screen.visibleFrame.midX - w.frame.width / 2, y: screen.visibleFrame.maxY))
        }
        let host = NSHostingView(rootView: LauncherView(m: model, done: { [weak self] in self?.finish($0) }, fit: { [weak self] in self?.fit($0) }))
        host.sizingOptions = []   // the Form's measured scroll content owns the height, including after collapse
        w.contentView = host
        host.layoutSubtreeIfNeeded()
        // no activation and no ordering in front of another app's windows: LaunchServices activates an app opened from Finder or the Dock (applicationDidBecomeActive), open -g does not
        if NSApp.isActive { w.makeKeyAndOrderFront(nil) } else { w.orderBack(nil) }
    }
    func applicationDidBecomeActive(_ n: Notification) { window?.makeKeyAndOrderFront(nil) }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {   // ⌘Q, or a logout or restart: the app ends without the game (exit 0, as Quit)
        fputs("launcher: quit without starting the game\n", stderr)
        return .terminateNow   // not terminateCancel, which a logout or restart takes as a refusal and stops
    }
    func windowWillClose(_ n: Notification) { finish(false) }

    func fit(_ extraHeight: CGFloat) {
        guard let w = window, let screen = w.screen else { return }
        let frame = LauncherWindowLayout.fitted(w.frame, extraHeight: extraHeight, visible: screen.visibleFrame)
        if abs(frame.height - w.frame.height) > 0.5 { w.setFrame(frame, display: true) }
    }

    func finish(_ play: Bool) {
        guard let w = window else { return }
        window = nil; self.play = play
        // The main thread blocks in launcher.c's waitpid once run() returns, so the window must leave the screen now: no fade-out (it runs on later run-loop passes),
        // and dmMacLauncher flushes the order-out (AppKit commits it at the end of a pass). Without both, the window stayed on screen, not responding, until the game ended
        w.animationBehavior = .none
        w.delegate = nil; w.orderOut(nil)
        NSApp.stop(nil)
        NSApp.postEvent(NSEvent.otherEvent(with: .applicationDefined, location: .zero, modifierFlags: [], timestamp: 0, windowNumber: 0, context: nil, subtype: 0, data1: 0, data2: 0)!, atStart: true)   // run returns at the next event
    }
}

enum LauncherWindowLayout {
    static func fitted(_ frame: NSRect, extraHeight: CGFloat, visible: NSRect) -> NSRect {
        let top = min(frame.maxY, visible.maxY)
        let height = min(frame.height + extraHeight, top - visible.minY)
        return NSRect(x: frame.minX, y: top - height, width: frame.width, height: height)
    }
}

// 1: Play (the window's settings are in the environment), 0: the player quit, 2: no window (no launcher.json, or "Show this window at launch" off): launcher.c applies the saved choices
@_cdecl("dm_ascension_migrate_preferences")
func migrateAscensionPreferences() {
    guard let domain = Bundle.main.bundleIdentifier,
          domain.hasPrefix("org.nxnative.oot.ascension.framegen") else { return }
    let defaults = UserDefaults.standard
    let marker = "ascension.preferencesMigrated.v1"
    guard !defaults.bool(forKey: marker) else { return }
    let allowed = Set(["window.appearance", "window.advanced", "window.show",
                       "DMSpatialAudioRouteV2", "DMSpeakerOutputPreference",
                       "DMShowFPS", "DMShowFPSOverlay"])
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"),
          let data = try? Data(contentsOf: url),
          let file = try? JSONDecoder().decode(SettingsFile.self, from: data) else { return }
    let settings = Dictionary(uniqueKeysWithValues: file.settings.map { ("launcher." + $0.env, $0) })
    // A fresh consolidation candidate inherits the latest playable choices;
    // only its own domain is written, and already-saved choices always win.
    let priorDomains = domain == "org.nxnative.oot.ascension.framegen.aspect20261114"
        ? ["org.nxnative.oot.ascension.framegen.final20261114", "org.nxnative.oot.ascension.framegen.finalcandidate",
           "org.nxnative.oot.ascension.framegen.consolidation", "org.nxnative.oot.ascension.framegen.gpustartup",
           "org.nxnative.oot.ascension.framegen.ultra", "org.nxnative.oot.ascension.release", "org.nxnative.oot.ascension.final"]
        : domain == "org.nxnative.oot.ascension.framegen.final20261114"
        ? ["org.nxnative.oot.ascension.framegen.finalcandidate", "org.nxnative.oot.ascension.framegen.consolidation",
           "org.nxnative.oot.ascension.framegen.gpustartup", "org.nxnative.oot.ascension.framegen.ultra",
           "org.nxnative.oot.ascension.release", "org.nxnative.oot.ascension.final"]
        : domain == "org.nxnative.oot.ascension.framegen.finalcandidate"
        ? ["org.nxnative.oot.ascension.framegen.consolidation", "org.nxnative.oot.ascension.framegen.gpustartup",
           "org.nxnative.oot.ascension.framegen.ultra", "org.nxnative.oot.ascension.release", "org.nxnative.oot.ascension.final"]
        : domain == "org.nxnative.oot.ascension.framegen.consolidation"
        ? ["org.nxnative.oot.ascension.framegen.gpustartup", "org.nxnative.oot.ascension.framegen.ultra",
           "org.nxnative.oot.ascension.release", "org.nxnative.oot.ascension.final"]
        : ["org.nxnative.oot.ascension.release", "org.nxnative.oot.ascension.final"]
    for oldDomain in priorDomains {
        for (key, value) in defaults.persistentDomain(forName: oldDomain) ?? [:] {
            guard defaults.object(forKey: key) == nil else { continue }
            if let setting = settings[key], let value = value as? String, setting.accepts(value) {
                defaults.set(value, forKey: key)
            } else if allowed.contains(key) {
                defaults.set(value, forKey: key)
            }
        }
    }
    defaults.set(true, forKey: marker)
}

@_cdecl("dm_mac_launcher")
public func dmMacLauncher() -> Int32 {
    guard let url = Bundle.main.resourceURL?.appendingPathComponent("dm/launcher.json"), let data = try? Data(contentsOf: url) else { return 2 }
    guard let file = try? JSONDecoder().decode(SettingsFile.self, from: data), !file.settings.isEmpty else { fputs("[RED] launcher: dm/launcher.json cannot be read; starting the game\n", stderr); return 2 }
    if (UserDefaults.standard.object(forKey: "window.show") as? Bool) == false && !NSEvent.modifierFlags.contains(.option) {
        fputs("launcher: settings window off (hold Option at launch to show it)\n", stderr)
        return SaveCheck.withoutWindow() ? 2 : 0
    }
    let model = LauncherModel(file.settings)
    let app = NSApplication.shared, launcher = Launcher(model)
    app.setActivationPolicy(.regular)
    app.delegate = launcher
    fputs("launcher: \(file.settings.count) settings shown\n", stderr)
    app.run()
    CATransaction.flush()   // finish's order-out
    app.setActivationPolicy(.prohibited)   // the host is the game's app from here on
    guard launcher.play else { fputs("launcher: quit without starting the game\n", stderr); return 0 }
    model.apply()
    return 1
}
