import AppKit
import SwiftUI
import UniformTypeIdentifiers

private struct Release: Decodable {
    let version: String

    static let bundled: Release? = {
        guard let url = Bundle.main.url(forResource: "Release", withExtension: "json"),
              let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(Release.self, from: data)
    }()
}

private struct InstallEvent: Decodable {
    let ev: String
    let name: String?
    let step: String?
    let msg: String?
    let path: String?
    let frac: Double?
}

@MainActor
private final class Installer: ObservableObject {
    @Published private(set) var nup: URL?
    @Published private(set) var folder: URL?
    @Published private(set) var result: URL?
    @Published private(set) var log: URL?
    @Published private(set) var stage = ""
    @Published private(set) var detail = ""
    @Published private(set) var failure = ""
    @Published private(set) var progress: Double?
    @Published private(set) var running = false
    @Published private(set) var installing = false
    @Published private(set) var cancelling = false
    private var process: Process?

    func chooseNUP() {
        let panel = NSOpenPanel()
        panel.prompt = "Choose NUP"
        panel.allowedContentTypes = [UTType(filenameExtension: "nup") ?? .data]
        guard panel.runModal() == .OK, let file = panel.url else { return }
        nup = file
        result = nil
        failure = ""
        if folder == nil {
            let volume = try? file.resourceValues(forKeys: [.volumeURLKey]).volume
            let nintendo = volume?.appendingPathComponent("Nintendo", isDirectory: true)
            if let nintendo, (try? nintendo.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) == true {
                folder = nintendo
            } else {
                folder = file.deletingLastPathComponent()
            }
        }
    }

    func chooseFolder() {
        let panel = NSOpenPanel()
        panel.prompt = "Choose Folder"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.directoryURL = folder
        guard panel.runModal() == .OK, let selected = panel.url else { return }
        folder = selected
        result = nil
        failure = ""
    }

    func start() {
        guard !running, let nup, let folder else { return }
        failure = ""
        result = nil
        log = nil
        progress = nil
        detail = ""
        stage = "Looking for build tools"
        running = true
        cancelling = false
        Task {
            defer {
                running = false
                installing = false
                cancelling = false
                process = nil
            }
            do {
                try await build(nup: nup, folder: folder)
            } catch {
                if failure.isEmpty { failure = error.localizedDescription }
            }
        }
    }

    func cancel() {
        guard !installing, !cancelling else { return }
        cancelling = true
        stage = "Stopping the build"
        process?.terminate()
    }

    private func build(nup: URL, folder: URL) async throws {
        guard let worker = Bundle.main.url(forResource: "install", withExtension: "py") else {
            throw InstallerFailure("This copy of Ascension Installer is missing its build tools.")
        }
        let environment = ProcessInfo.processInfo.environment
        let inherited = (environment["PATH"] ?? "").split(separator: ":").map(String.init)
        let search = ["/opt/homebrew/bin"] + inherited + ["/usr/local/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"]
        let python = try await Task.detached { try Self.findPython(in: search) }.value
        if cancelling { throw InstallerFailure("Build cancelled.") }
        let process = Process()
        let output = Pipe()
        process.executableURL = python
        process.arguments = ["-u", worker.path, nup.path, folder.path]
        process.environment = environment.merging(["PATH": search.joined(separator: ":")]) { _, path in path }
        process.standardOutput = output
        process.standardError = output
        try process.run()
        self.process = process
        let decoder = JSONDecoder()
        for try await line in output.fileHandleForReading.bytes.lines {
            guard let update = try? decoder.decode(InstallEvent.self, from: Data(line.utf8)) else { continue }
            switch update.ev {
            case "stage", "step":
                stage = update.name ?? stage
                detail = update.step ?? ""
                progress = nil
            case "progress":
                stage = update.name ?? stage
                detail = update.step ?? ""
                progress = update.frac
            case "installing":
                installing = true
                stage = update.name ?? "Installing Ascension"
                progress = nil
                detail = ""
            case "say":
                if let message = update.msg { detail = message }
            case "log":
                if let path = update.path { log = URL(fileURLWithPath: path) }
            case "installed":
                if let path = update.path { result = URL(fileURLWithPath: path) }
            case "error":
                failure = update.msg ?? "The build stopped."
            default:
                break
            }
        }
        process.waitUntilExit()
        if process.terminationStatus != 0 || result == nil {
            throw InstallerFailure(cancelling ? "Build cancelled." : "The build stopped. See the build log for details.")
        }
    }

    private nonisolated static func findPython(in directories: [String]) throws -> URL {
        let files = FileManager.default
        for directory in directories {
            let python = URL(fileURLWithPath: directory).appendingPathComponent("python3")
            guard files.isExecutableFile(atPath: python.path) else { continue }
            let check = Process()
            check.executableURL = python
            check.arguments = ["-c", "import platform, sys; sys.exit(sys.version_info < (3, 14) or platform.machine() != 'arm64')"]
            check.standardOutput = FileHandle.nullDevice
            check.standardError = FileHandle.nullDevice
            guard (try? check.run()) != nil else { continue }
            check.waitUntilExit()
            if check.terminationStatus == 0 { return python }
        }
        throw InstallerFailure("Install Apple silicon Python 3.14 or later with Homebrew, then try again.")
    }
}

private struct InstallerFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

@main
struct AscensionInstaller: App {
    @StateObject private var installer = Installer()

    var body: some Scene {
        Window("Ascension Installer", id: "installer") {
            VStack(alignment: .leading, spacing: 22) {
                HStack(spacing: 15) {
                    Image(systemName: "shippingbox.fill")
                        .symbolRenderingMode(.hierarchical)
                        .font(.system(size: 36))
                        .foregroundStyle(.tint)
                        .accessibilityHidden(true)
                    VStack(alignment: .leading, spacing: 5) {
                        Text("Ascension \(Release.bundled?.version ?? "")").font(.title.bold())
                        Text("Ocarina of Time for Mac").foregroundStyle(.secondary)
                    }
                }
                Divider()
                VStack(alignment: .leading, spacing: 15) {
                    selection("Switch 2 NUP", value: installer.nup?.lastPathComponent, fallback: "Ocarina of Time · v131072",
                              button: "Choose NUP…", action: installer.chooseNUP)
                    selection("Install folder", value: installer.folder?.path, fallback: "Choose where the Mac app will go",
                              button: "Choose Folder…", action: installer.chooseFolder)
                }
                .disabled(installer.running)
                if let result = installer.result {
                    Label("Ascension is installed.", systemImage: "checkmark.circle.fill")
                        .font(.headline).foregroundStyle(.green)
                    Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([result]) }
                } else if installer.running {
                    VStack(alignment: .leading, spacing: 7) {
                        Text(installer.stage).font(.headline)
                        if let progress = installer.progress {
                            ProgressView(value: progress)
                        } else {
                            ProgressView()
                        }
                        if !installer.detail.isEmpty {
                            Text(installer.detail).font(.callout).foregroundStyle(.secondary).lineLimit(3)
                        }
                    }
                } else if !installer.failure.isEmpty {
                    Label(installer.failure, systemImage: "exclamationmark.circle")
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                } else {
                    Text("First build requires full Xcode, its Metal Toolchain, and Homebrew build tools. Shader compilation can take several hours.")
                        .font(.callout).foregroundStyle(.secondary)
                }
                HStack {
                    if let log = installer.log, FileManager.default.fileExists(atPath: log.path) {
                        Button("Open Build Log") { NSWorkspace.shared.open(log) }
                    }
                    Spacer()
                    if installer.running {
                        Button("Cancel Build", action: installer.cancel)
                            .disabled(installer.installing || installer.cancelling)
                    } else {
                        Button("Build & Install", action: installer.start)
                            .keyboardShortcut(.defaultAction)
                            .buttonStyle(.borderedProminent)
                            .disabled(installer.nup == nil || installer.folder == nil || Release.bundled == nil)
                    }
                }
            }
            .padding(28)
            .frame(width: 560)
        }
        .windowResizability(.contentSize)
    }

    private func selection(_ title: String, value: String?, fallback: String, button: String, action: @escaping () -> Void) -> some View {
        HStack(alignment: .center, spacing: 14) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                Text(value ?? fallback).foregroundStyle(.secondary).lineLimit(2).truncationMode(.middle)
            }
            Spacer()
            Button(button, action: action)
        }
    }
}
