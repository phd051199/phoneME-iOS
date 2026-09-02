import CoreGraphics
import Foundation
import OSLog

private let phoneMEMultitaskingLogger = Logger(
    subsystem: "dev.phoneme.emulator",
    category: "J2MEMultitasking"
)

enum EmulatorState: Equatable {
    case idle
    case starting
    case running
    case stopped
    case failed(String)

    var title: String {
        switch self {
        case .idle: return "Idle"
        case .starting: return "Starting"
        case .running: return "Running"
        case .stopped: return "Stopped"
        case .failed: return "Error"
        }
    }
}

@MainActor
final class EmbeddedPhoneMEEngine: NSObject {
    var onFrame: ((PhoneMEFrame) -> Void)?
    var onLCDUIEvents: (([PhoneMECAPI.LCDUIEvent]) -> Void)?
    var onLCDUIImages: (([Int32: CGImage]) -> Void)?
    var onStateChange: ((EmulatorState) -> Void)?
    var onFPSChange: ((Double) -> Void)?
    var onApplicationStateChange: ((UUID, PhoneMECAPI.AppState) -> Void)?
    var onForegroundApplicationChange: ((UUID?) -> Void)?

    private final class RuntimeContext: @unchecked Sendable {
        var api: PhoneMECAPI?
        var runtime: PhoneMECAPI.RuntimeHandle?
        var suiteIDs: [UUID: Int32] = [:]
        var appIDs: [UUID: Int32] = [:]
        var gameIDsByAppID: [Int32: UUID] = [:]
        var nextAppID: Int32 = 1

        func nextAvailableAppID() -> Int32? {
            guard gameIDsByAppID.count < 64, nextAppID > 0 else {
                return nil
            }

            // Component IDs are namespaced by appID in Core. Never recycle an
            // appID while this runtime context is alive, otherwise a delayed
            // LCDUI action from a destroyed app could target a component in a
            // later app that inherited the same namespace.
            let candidate = nextAppID
            if candidate == Int32.max {
                nextAppID = 0
            } else {
                nextAppID = candidate + 1
            }
            return candidate
        }

        func removeApplication(gameID: UUID) {
            guard let appID = appIDs.removeValue(forKey: gameID) else {
                return
            }
            gameIDsByAppID.removeValue(forKey: appID)
        }

        func reset() {
            api = nil
            runtime = nil
            suiteIDs.removeAll(keepingCapacity: true)
            appIDs.removeAll(keepingCapacity: true)
            gameIDsByAppID.removeAll(keepingCapacity: true)
            nextAppID = 1
        }
    }

    private final class LaunchToken: @unchecked Sendable {
        private let lock = NSLock()
        private var cancelled = false

        var isCancelled: Bool {
            lock.lock()
            let value = cancelled
            lock.unlock()
            return value
        }

        func cancel() {
            lock.lock()
            cancelled = true
            lock.unlock()
        }
    }

    private struct VisibleScreen: Equatable, Sendable {
        let id: Int32
        let componentType: Int32

        var usesNativeLCDUI: Bool {
            componentType != LCDUIState.ComponentType.canvas.rawValue
        }
    }

    private final class ContinuousInputBuffer: @unchecked Sendable {
        struct TextUpdate: Sendable {
            let text: String
            let caretPosition: Int
        }

        struct PointerUpdate: Sendable {
            let x: Int32
            let y: Int32
        }

        struct Snapshot: Sendable {
            let textUpdates: [Int32: TextUpdate]
            let gaugeUpdates: [Int32: Int]
            let dateUpdates: [Int32: Date]
            let scrollPosition: Int?
            let pointerMove: PointerUpdate?
        }

        private let lock = NSLock()
        private var textUpdates: [Int32: TextUpdate] = [:]
        private var gaugeUpdates: [Int32: Int] = [:]
        private var dateUpdates: [Int32: Date] = [:]
        private var scrollPosition: Int?
        private var pointerMove: PointerUpdate?
        private var flushScheduled = false

        func submitText(
            componentID: Int32,
            text: String,
            caretPosition: Int
        ) -> Bool {
            lock.lock()
            textUpdates[componentID] = TextUpdate(
                text: text,
                caretPosition: caretPosition
            )
            let shouldSchedule = markFlushScheduledLocked()
            lock.unlock()
            return shouldSchedule
        }

        func submitGauge(componentID: Int32, value: Int) -> Bool {
            lock.lock()
            gaugeUpdates[componentID] = value
            let shouldSchedule = markFlushScheduledLocked()
            lock.unlock()
            return shouldSchedule
        }

        func submitDate(componentID: Int32, date: Date) -> Bool {
            lock.lock()
            dateUpdates[componentID] = date
            let shouldSchedule = markFlushScheduledLocked()
            lock.unlock()
            return shouldSchedule
        }

        func submitScrollPosition(_ position: Int) -> Bool {
            lock.lock()
            scrollPosition = position
            let shouldSchedule = markFlushScheduledLocked()
            lock.unlock()
            return shouldSchedule
        }

        func submitPointerMove(x: Int32, y: Int32) -> Bool {
            lock.lock()
            pointerMove = PointerUpdate(x: x, y: y)
            let shouldSchedule = markFlushScheduledLocked()
            lock.unlock()
            return shouldSchedule
        }

        func takeSnapshot() -> Snapshot {
            lock.lock()
            let snapshot = Snapshot(
                textUpdates: textUpdates,
                gaugeUpdates: gaugeUpdates,
                dateUpdates: dateUpdates,
                scrollPosition: scrollPosition,
                pointerMove: pointerMove
            )
            textUpdates.removeAll(keepingCapacity: true)
            gaugeUpdates.removeAll(keepingCapacity: true)
            dateUpdates.removeAll(keepingCapacity: true)
            scrollPosition = nil
            pointerMove = nil
            flushScheduled = false
            lock.unlock()
            return snapshot
        }

        func reset() {
            lock.lock()
            textUpdates.removeAll(keepingCapacity: true)
            gaugeUpdates.removeAll(keepingCapacity: true)
            dateUpdates.removeAll(keepingCapacity: true)
            scrollPosition = nil
            pointerMove = nil
            flushScheduled = false
            lock.unlock()
        }

        private func markFlushScheduledLocked() -> Bool {
            guard !flushScheduled else { return false }
            flushScheduled = true
            return true
        }
    }

    private final class PollContext: @unchecked Sendable {
        struct TelemetrySnapshot: Sendable {
            let durationSeconds: Double
            let callbacks: UInt64
            let activeCallbacks: UInt64
            let idleCanvasCallbacks: UInt64
            let idleNativeCallbacks: UInt64
            let framesProduced: UInt64
            let targetFramesPerSecond: UInt64
        }

        var visibleScreen: VisibleScreen?
        var didReportRuntimeExit = false

        private var frameIntervalNanoseconds: UInt64
        // Core now pushes a host-wake edge for repaint/frame/LCDUI changes.
        // Keep only a slow watchdog timer for missed/third-party edges instead
        // of waking 10-30 times/sec while a game screen is static.
        let idleNativePollIntervalNanoseconds: UInt64 = 500_000_000
        let idleCanvasPollIntervalNanoseconds: UInt64
        let idleNativePollThreshold: Int
        let idleCanvasPollThreshold: Int

        private let cadenceLock = NSLock()
        private var consecutiveIdleNativePolls = 0
        private var consecutiveIdleCanvasPolls = 0
        private var usesIdleNativeCadence = false
        private var usesIdleCanvasCadence = false
        private let configuredFramesPerSecond: UInt64
        private var framesPerSecond: UInt64
        private var wholeFrameIntervalNanoseconds: UInt64
        private var frameIntervalRemainder: UInt64
        private var remainderAccumulator: UInt64 = 0
        private var nextActiveDeadlineNanoseconds: UInt64 = 0
        private var telemetryStartedNanoseconds: UInt64 = 0
        private var telemetryCallbacks: UInt64 = 0
        private var telemetryActiveCallbacks: UInt64 = 0
        private var telemetryIdleCanvasCallbacks: UInt64 = 0
        private var telemetryIdleNativeCallbacks: UInt64 = 0
        private var telemetryFramesProduced: UInt64 = 0

        init(
            framesPerSecond: Int,
            thermalPressure: PhoneMECAPI.ThermalPressure
        ) {
            let normalizedFrameRate = GameProfile.resolvedFrameRate(
                framesPerSecond
            )
            configuredFramesPerSecond = UInt64(normalizedFrameRate)
            let effectiveFrameRate = Self.effectiveFramesPerSecond(
                configured: configuredFramesPerSecond,
                thermalPressure: thermalPressure
            )
            self.framesPerSecond = effectiveFrameRate
            wholeFrameIntervalNanoseconds = 1_000_000_000
                / effectiveFrameRate
            frameIntervalRemainder = 1_000_000_000
                % effectiveFrameRate
            frameIntervalNanoseconds = (
                1_000_000_000 + effectiveFrameRate / 2
            ) / effectiveFrameRate
            idleCanvasPollIntervalNanoseconds = max(
                frameIntervalNanoseconds * 4,
                500_000_000
            )
            idleNativePollThreshold = 3
            idleCanvasPollThreshold = max(normalizedFrameRate / 8, 3)
        }

        func updateThermalPressure(
            _ thermalPressure: PhoneMECAPI.ThermalPressure
        ) {
            cadenceLock.lock()
            let nextFrameRate = Self.effectiveFramesPerSecond(
                configured: configuredFramesPerSecond,
                thermalPressure: thermalPressure
            )
            guard nextFrameRate != framesPerSecond else {
                cadenceLock.unlock()
                return
            }
            framesPerSecond = nextFrameRate
            wholeFrameIntervalNanoseconds = 1_000_000_000 / nextFrameRate
            frameIntervalRemainder = 1_000_000_000 % nextFrameRate
            frameIntervalNanoseconds = (
                1_000_000_000 + nextFrameRate / 2
            ) / nextFrameRate
            // Restart from wall time rather than carrying a 60 FPS remainder
            // into a 30/20 FPS cadence (or vice versa). This prevents a short
            // catch-up burst exactly when iOS has asked us to reduce heat.
            nextActiveDeadlineNanoseconds = 0
            remainderAccumulator = 0
            cadenceLock.unlock()
        }

        var pollLeewayNanoseconds: UInt64 {
            cadenceLock.lock()
            defer { cadenceLock.unlock() }
            if usesIdleNativeCadence {
                return 10_000_000
            }
            if usesIdleCanvasCadence {
                return 6_000_000
            }
            return min(
                max(frameIntervalNanoseconds / 16, 500_000),
                2_000_000
            )
        }

        func updateNativeIdle(_ isIdle: Bool) {
            cadenceLock.lock()
            if isIdle {
                consecutiveIdleNativePolls += 1
                if consecutiveIdleNativePolls >= idleNativePollThreshold {
                    usesIdleNativeCadence = true
                }
                consecutiveIdleCanvasPolls = 0
                usesIdleCanvasCadence = false
            } else {
                consecutiveIdleNativePolls = 0
                usesIdleNativeCadence = false
            }
            cadenceLock.unlock()
        }

        func noteCanvasFrameOutcome(producedFrame: Bool) {
            cadenceLock.lock()
            if producedFrame {
                telemetryFramesProduced &+= 1
                consecutiveIdleCanvasPolls = 0
                usesIdleCanvasCadence = false
                nextActiveDeadlineNanoseconds = 0
                remainderAccumulator = 0
            } else if !usesIdleNativeCadence {
                consecutiveIdleCanvasPolls += 1
                if consecutiveIdleCanvasPolls >= idleCanvasPollThreshold {
                    usesIdleCanvasCadence = true
                }
            }
            cadenceLock.unlock()
        }

        func notePollForTelemetry(
            nowNanoseconds: UInt64
        ) -> TelemetrySnapshot? {
            cadenceLock.lock()
            defer { cadenceLock.unlock() }
            if telemetryStartedNanoseconds == 0 {
                telemetryStartedNanoseconds = nowNanoseconds
            }
            telemetryCallbacks &+= 1
            if usesIdleNativeCadence {
                telemetryIdleNativeCallbacks &+= 1
            } else if usesIdleCanvasCadence {
                telemetryIdleCanvasCallbacks &+= 1
            } else {
                telemetryActiveCallbacks &+= 1
            }
            let elapsed = nowNanoseconds &- telemetryStartedNanoseconds
            guard elapsed >= 5_000_000_000 else { return nil }
            let snapshot = TelemetrySnapshot(
                durationSeconds: Double(elapsed) / 1.0e9,
                callbacks: telemetryCallbacks,
                activeCallbacks: telemetryActiveCallbacks,
                idleCanvasCallbacks: telemetryIdleCanvasCallbacks,
                idleNativeCallbacks: telemetryIdleNativeCallbacks,
                framesProduced: telemetryFramesProduced,
                targetFramesPerSecond: framesPerSecond
            )
            telemetryStartedNanoseconds = nowNanoseconds
            telemetryCallbacks = 0
            telemetryActiveCallbacks = 0
            telemetryIdleCanvasCallbacks = 0
            telemetryIdleNativeCallbacks = 0
            telemetryFramesProduced = 0
            return snapshot
        }

        func noteHostActivity() {
            cadenceLock.lock()
            consecutiveIdleCanvasPolls = 0
            usesIdleCanvasCadence = false
            nextActiveDeadlineNanoseconds = 0
            remainderAccumulator = 0
            cadenceLock.unlock()
        }

        func nextPollDeadline(after nowNanoseconds: UInt64) -> DispatchTime {
            cadenceLock.lock()
            defer { cadenceLock.unlock() }
            if usesIdleNativeCadence {
                nextActiveDeadlineNanoseconds = 0
                remainderAccumulator = 0
                return DispatchTime(
                    uptimeNanoseconds: nowNanoseconds
                        + idleNativePollIntervalNanoseconds
                )
            }
            if usesIdleCanvasCadence {
                nextActiveDeadlineNanoseconds = 0
                remainderAccumulator = 0
                return DispatchTime(
                    uptimeNanoseconds: nowNanoseconds
                        + idleCanvasPollIntervalNanoseconds
                )
            }

            if nextActiveDeadlineNanoseconds == 0 {
                nextActiveDeadlineNanoseconds = nowNanoseconds
            }
            advanceActiveDeadline()

            // A delayed callback must never trigger a burst of catch-up pumps.
            // Re-anchor one frame ahead of wall time instead; Java clocks and
            // Timer/Thread.sleep continue to observe real elapsed time only.
            if nextActiveDeadlineNanoseconds <= nowNanoseconds {
                nextActiveDeadlineNanoseconds = nowNanoseconds
                remainderAccumulator = 0
                advanceActiveDeadline()
            }

            return DispatchTime(
                uptimeNanoseconds: nextActiveDeadlineNanoseconds
            )
        }

        private func advanceActiveDeadline() {
            nextActiveDeadlineNanoseconds += wholeFrameIntervalNanoseconds
            remainderAccumulator += frameIntervalRemainder
            if remainderAccumulator >= framesPerSecond {
                nextActiveDeadlineNanoseconds += 1
                remainderAccumulator -= framesPerSecond
            }
        }

        private static func effectiveFramesPerSecond(
            configured: UInt64,
            thermalPressure: PhoneMECAPI.ThermalPressure
        ) -> UInt64 {
            switch thermalPressure {
            case .critical:
                return min(configured, 20)
            case .serious:
                return min(configured, 30)
            case .fair, .nominal:
                return configured
            }
        }
    }

    private final class RenderRequestBuffer: @unchecked Sendable {
        struct Snapshot: Sendable {
            let imageComponentIDs: Set<Int32>
            let visibleScreen: VisibleScreen?
            let wantsFrame: Bool
            let previousFrameGeneration: UInt64
        }

        private let lock = NSLock()
        private var pendingImageComponentIDs = Set<Int32>()
        private var pendingVisibleScreen: VisibleScreen?
        private var pendingFrame = false
        private var hasPendingRequest = false
        private var workerRunning = false
        private var frameGeneration: UInt64 = 0

        func submit(
            imageComponentIDs: Set<Int32>,
            visibleScreen: VisibleScreen?,
            wantsFrame: Bool
        ) -> Bool {
            lock.lock()
            pendingImageComponentIDs.formUnion(imageComponentIDs)
            pendingVisibleScreen = visibleScreen
            pendingFrame = pendingFrame || wantsFrame
            hasPendingRequest = true
            let shouldStartWorker = !workerRunning
            workerRunning = true
            lock.unlock()
            return shouldStartWorker
        }

        func takeSnapshot() -> Snapshot {
            lock.lock()
            let snapshot = Snapshot(
                imageComponentIDs: pendingImageComponentIDs,
                visibleScreen: pendingVisibleScreen,
                wantsFrame: pendingFrame,
                previousFrameGeneration: frameGeneration
            )
            pendingImageComponentIDs.removeAll(keepingCapacity: true)
            pendingFrame = false
            hasPendingRequest = false
            lock.unlock()
            return snapshot
        }

        func updateFrameGeneration(_ generation: UInt64) {
            lock.lock()
            frameGeneration = generation
            lock.unlock()
        }

        func invalidateFrameGeneration() {
            lock.lock()
            frameGeneration = 0
            lock.unlock()
        }

        func completeIteration() -> Bool {
            lock.lock()
            let shouldContinue = hasPendingRequest
            if !shouldContinue {
                workerRunning = false
            }
            lock.unlock()
            return shouldContinue
        }
    }

    private let runtimeQueue = DispatchQueue(
        label: "com.phoneme.runtime.lifecycle",
        // Guest lifecycle callbacks may become the MIDlet's long-running
        // execution loop when a game does not spawn a separate Thread. Keep
        // them off userInitiated/performance-core QoS; input and presentation
        // have their own latency-sensitive queues.
        qos: .utility
    )
    private let inputQueue = DispatchQueue(
        label: "com.phoneme.runtime.input",
        qos: .userInteractive
    )
    private let pollQueue = DispatchQueue(
        label: "com.phoneme.runtime.lcdui-poll",
        // This queue drives presentation cadence. Utility QoS can be delayed
        // several milliseconds under load, which is visible as uneven frame
        // delivery in heavy games even when the VM itself finishes on time.
        qos: .userInitiated
    )
    private let renderQueue = DispatchQueue(
        label: "com.phoneme.runtime.render",
        qos: .userInitiated
    )
    private let runtimeContext = RuntimeContext()
    private var api: PhoneMECAPI?
    private var runtime: PhoneMECAPI.RuntimeHandle?
    private var foregroundGameID: UUID?
    private var foregroundAppID: Int32?
    private var pendingForegroundGameID: UUID?
    private var launchToken = LaunchToken()
    private var pollTimer: DispatchSourceTimer?
    private var pollTimerIsSuspended = false
    private let activePollContextLock = NSLock()
    private var activePollContext: PollContext?
    private var runtimeIsSuspended = false
    private var runtimeSuspensionRequested = false
    private var shouldRunInForeground = true
    private var launchIdentifier = UUID()
    private let continuousInputBuffer = ContinuousInputBuffer()
    private var renderRequestBuffer = RenderRequestBuffer()
    private var framePollingFramesPerSecond = GameProfile.defaultFrameRate
    private var fpsFrameCount = 0
    private var fpsMeasurementStartNanoseconds = DispatchTime.now()
        .uptimeNanoseconds

    override init() {
        super.init()
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleThermalStateDidChange),
            name: ProcessInfo.thermalStateDidChangeNotification,
            object: nil
        )
        // Prepare the immutable runtime payload once. All access to the C ABI
        // is serialized through this same queue; the VM itself owns its worker
        // pthread, so another Swift-side worker layer is unnecessary.
        runtimeQueue.async {
            _ = try? PhoneMERuntimeResources.prepare()
        }
    }

    @objc private func handleThermalStateDidChange() {
        applyCurrentThermalPressure()
    }

    private func applyCurrentThermalPressure() {
        let pressure = PhoneMECAPI.currentThermalPressure
        activePollContextLock.lock()
        let pollContext = activePollContext
        activePollContextLock.unlock()
        pollContext?.updateThermalPressure(pressure)
        guard let api, let runtime else { return }
        runtimeQueue.async {
            _ = api.setThermalPressure(runtime, pressure: pressure)
        }
    }

    func prepareApplications(_ applications: [UUID: URL]) {
        guard !applications.isEmpty else { return }
        let context = runtimeContext

        runtimeQueue.async { [weak self] in
            do {
                let (loadedAPI, createdRuntime) = try Self.ensureRuntime(
                    in: context
                )

                // Bulk library preparation can transiently allocate several
                // large ZIP/class indexes. Never do that beside a live game;
                // a newly imported suite is installed on demand by its launch
                // path, which remains safe while unrelated MIDlets are active.
                guard !loadedAPI.isRunning(createdRuntime) else { return }

                for (gameID, jarURL) in applications
                    where context.suiteIDs[gameID] == nil {
                    let install = loadedAPI.installJar(
                        createdRuntime,
                        jarURL: jarURL,
                        identityNamespace: gameID.uuidString.lowercased()
                    )
                    phoneMEMultitaskingLogger.info(
                        "Prepared game \(gameID.uuidString, privacy: .public): status=\(install.status), suite=\(install.suiteID ?? 0), installerStage=\(loadedAPI.lastInstallStage()), storeStage=\(loadedAPI.lastSuiteStoreStage())"
                    )
                    guard install.status == 0, let suiteID = install.suiteID else {
                        // A corrupt or unsupported JAR must not abort preparation
                        // for every other library item. Keep it unmapped so a
                        // later explicit launch reports the error for this game
                        // only, while valid suites remain available to MVM.
                        phoneMEMultitaskingLogger.error(
                            "Skipped game \(gameID.uuidString, privacy: .public) during preparation: status=\(install.status), installerStage=\(loadedAPI.lastInstallStage()), storeStage=\(loadedAPI.lastSuiteStoreStage())"
                        )
                        continue
                    }
                    let trustResult = loadedAPI.setSuiteTrusted(
                        createdRuntime,
                        suiteID: suiteID
                    )
                    guard trustResult == 0 else {
                        phoneMEMultitaskingLogger.error(
                            "Skipped game \(gameID.uuidString, privacy: .public) because suite trust failed: status=\(trustResult)"
                        )
                        continue
                    }
                    context.suiteIDs[gameID] = suiteID
                }

                DispatchQueue.main.async {
                    guard let self else { return }
                    self.api = loadedAPI
                    self.runtime = createdRuntime
                    self.applyCurrentThermalPressure()
                }
            } catch {
                // Library preparation is opportunistic. Do not poison the
                // foreground session if runtime setup fails here; launching a
                // selected app will retry and surface a game-specific error.
                phoneMEMultitaskingLogger.error(
                    "Preparation failed: \(error.localizedDescription, privacy: .public)"
                )
            }
        }
    }

    func exportRMS(
        gameID: UUID,
        jarURL: URL,
        completion: @escaping (Result<Data, Error>) -> Void
    ) {
        let context = runtimeContext
        runtimeQueue.async {
            let result: Result<Data, Error>
            do {
                guard context.appIDs.isEmpty else {
                    throw PhoneMERMSBackupError.runtimeBusy
                }
                let (loadedAPI, createdRuntime) = try Self.ensureRuntime(
                    in: context
                )
                let suiteID = try Self.resolveSuiteID(
                    gameID: gameID,
                    jarURL: jarURL,
                    api: loadedAPI,
                    runtime: createdRuntime,
                    context: context
                )
                result = .success(
                    try PhoneMERuntimeResources.exportRMSBackup(
                        suiteID: suiteID,
                        jarURL: jarURL
                    )
                )
            } catch {
                result = .failure(error)
            }
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    func importRMS(
        from sourceURL: URL,
        gameID: UUID,
        jarURL: URL,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let context = runtimeContext
        runtimeQueue.async {
            let result: Result<Void, Error>
            do {
                guard context.appIDs.isEmpty else {
                    throw PhoneMERMSBackupError.runtimeBusy
                }
                let (loadedAPI, createdRuntime) = try Self.ensureRuntime(
                    in: context
                )
                let suiteID = try Self.resolveSuiteID(
                    gameID: gameID,
                    jarURL: jarURL,
                    api: loadedAPI,
                    runtime: createdRuntime,
                    context: context
                )
                try PhoneMERuntimeResources.importRMSBackup(
                    from: sourceURL,
                    suiteID: suiteID,
                    jarURL: jarURL
                )
                result = .success(())
            } catch {
                result = .failure(error)
            }
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    func uninstallApplication(
        gameID: UUID,
        jarURL: URL,
        removeData: Bool,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let context = runtimeContext
        runtimeQueue.async { [weak self] in
            let result: Result<Void, Error>
            do {
                let (loadedAPI, createdRuntime) = try Self.ensureRuntime(
                    in: context
                )
                let suiteID = try Self.resolveSuiteID(
                    gameID: gameID,
                    jarURL: jarURL,
                    api: loadedAPI,
                    runtime: createdRuntime,
                    context: context
                )
                let status = loadedAPI.uninstallSuite(
                    createdRuntime,
                    suiteID: suiteID,
                    removeData: removeData
                )
                guard status == PHONEME_OK else {
                    throw loadedAPI.failure(
                        status: status,
                        runtime: createdRuntime
                    )
                }
                context.suiteIDs.removeValue(forKey: gameID)

                if context.appIDs.isEmpty {
                    loadedAPI.stop(createdRuntime)
                    loadedAPI.destroyRuntime(createdRuntime)
                    context.reset()
                }
                result = .success(())
            } catch {
                result = .failure(error)
            }

            DispatchQueue.main.async {
                if context.runtime == nil {
                    self?.api = nil
                    self?.runtime = nil
                    self?.runtimeIsSuspended = false
                    self?.runtimeSuspensionRequested = false
                }
                completion(result)
            }
        }
    }

    func launchApplication(
        gameID: UUID,
        jarURL: URL,
        mainClass: String,
        mediaTitle: String,
        mediaArtist: String,
        suiteVersion: String,
        mediaArtworkPath: String?,
        screenWidth: Int,
        screenHeight: Int,
        frameRateLimit: Int,
        framePacingMode: GameProfile.FramePacingMode,
        heapSizeMegabytes: Int,
        autoTranslateToVietnamese: Bool,
        translationProvider: TranslationProvider,
        translationSourceLanguage: TranslationSourceLanguage,
        keyUp: Int32,
        keyDown: Int32,
        keyLeft: Int32,
        keyRight: Int32,
        keyFire: Int32,
        keySoftLeft: Int32,
        keySoftRight: Int32
    ) {
        let requestedFPS = GameProfile.resolvedFrameRate(
            frameRateLimit,
            mode: framePacingMode
        )
        let requestedHeapSizeMegabytes =
            GameProfile.resolvedHeapSizeMegabytes(heapSizeMegabytes)
        framePollingFramesPerSecond = requestedFPS
        fpsMeasurementStartNanoseconds = DispatchTime.now()
            .uptimeNanoseconds

        let previousPendingGameID = pendingForegroundGameID
        launchToken.cancel()
        let currentLaunchToken = LaunchToken()
        launchToken = currentLaunchToken

        let currentLaunchIdentifier = UUID()
        launchIdentifier = currentLaunchIdentifier

        let previousForegroundGameID = foregroundGameID
        stopForegroundPolling()
        foregroundGameID = nil
        foregroundAppID = nil
        pendingForegroundGameID = gameID
        if let previousPendingGameID, previousPendingGameID != gameID {
            onApplicationStateChange?(previousPendingGameID, .active)
        }
        if previousForegroundGameID != nil {
            onForegroundApplicationChange?(nil)
        }
        setState(.starting)

        let context = runtimeContext
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async { [weak self] in
            do {
                // A foreground transition owns the process-global LCDUI and
                // framebuffer bridges. Wait for every operation submitted by
                // the previous screen before changing the NAMS foreground app.
                inputQueue.sync { }
                pollQueue.sync { }
                renderQueue.sync { }
                guard !currentLaunchToken.isCancelled else { return }

                let (loadedAPI, createdRuntime) = try Self.ensureRuntime(
                    in: context
                )

                let resolvedSuiteID: Int32
                if let preparedSuiteID = context.suiteIDs[gameID] {
                    resolvedSuiteID = preparedSuiteID
                } else if let installedSuiteID = loadedAPI.findInstalledSuite(
                    createdRuntime,
                    vendor: mediaArtist,
                    name: mediaTitle,
                    version: suiteVersion,
                    identityNamespace: gameID.uuidString.lowercased()
                ) {
                    // A fresh iOS process starts with an empty in-memory
                    // game->suite map even though SuiteStore has already
                    // recovered the managed suite database. Reuse that suite
                    // by its persisted MIDP identity instead of inspecting,
                    // hashing and comparing the original imported JAR again
                    // on every launch.
                    context.suiteIDs[gameID] = installedSuiteID
                    resolvedSuiteID = installedSuiteID
                } else {
                    let install = loadedAPI.installJar(
                        createdRuntime,
                        jarURL: jarURL,
                        identityNamespace: gameID.uuidString.lowercased()
                    )
                    phoneMEMultitaskingLogger.info(
                        "Prepared launch game \(gameID.uuidString, privacy: .public): status=\(install.status), suite=\(install.suiteID ?? 0), installerStage=\(loadedAPI.lastInstallStage()), storeStage=\(loadedAPI.lastSuiteStoreStage())"
                    )
                    guard install.status == 0, let installedSuiteID = install.suiteID else {
                        throw loadedAPI.failure(
                            status: install.status,
                            runtime: createdRuntime
                        )
                    }
                    context.suiteIDs[gameID] = installedSuiteID
                    resolvedSuiteID = installedSuiteID
                }
                // Trust is persisted with the installed suite, but reassert it
                // on every launch so prepared/reused suites cannot regress to
                // the untrusted domain after runtime or suite-store recovery.
                let trustResult = loadedAPI.setSuiteTrusted(
                    createdRuntime,
                    suiteID: resolvedSuiteID
                )
                guard trustResult == 0 else {
                    throw loadedAPI.failure(
                        status: trustResult,
                        runtime: createdRuntime
                    )
                }

                guard !currentLaunchToken.isCancelled else { return }

                if !loadedAPI.isRunning(createdRuntime) {
                    let systemStartResult = loadedAPI.startSystem(createdRuntime)
                    phoneMEMultitaskingLogger.info(
                        "Started C++ runtime system: status=\(systemStartResult)"
                    )
                    guard systemStartResult == 0 else {
                        throw loadedAPI.failure(
                            status: systemStartResult,
                            runtime: createdRuntime
                        )
                    }
                }

                let translationResult = loadedAPI.configureTranslation(
                    createdRuntime,
                    enabled: autoTranslateToVietnamese,
                    provider: translationProvider,
                    sourceLanguage: translationSourceLanguage
                )
                guard translationResult == 0 else {
                    throw loadedAPI.failure(
                        status: translationResult,
                        runtime: createdRuntime
                    )
                }

                let keymapResult = loadedAPI.configureKeymap(
                    createdRuntime,
                    up: keyUp,
                    down: keyDown,
                    left: keyLeft,
                    right: keyRight,
                    fire: keyFire,
                    softLeft: keySoftLeft,
                    softRight: keySoftRight
                )
                guard keymapResult == 0 else {
                    throw loadedAPI.failure(
                        status: keymapResult,
                        runtime: createdRuntime
                    )
                }

                let configureFramePacing: (Int32) throws -> Void = { appID in
                    let result = loadedAPI.configureApplicationFramePacing(
                        createdRuntime,
                        appID: appID,
                        framesPerSecond: requestedFPS,
                        mode: framePacingMode
                    )
                    guard result == 0 else {
                        throw loadedAPI.failure(
                            status: result,
                            runtime: createdRuntime,
                            appID: appID
                        )
                    }
                }
                let configureHeap: (Int32) throws -> Void = { appID in
                    let result = loadedAPI.configureApplicationHeap(
                        createdRuntime,
                        appID: appID,
                        heapMegabytes: requestedHeapSizeMegabytes
                    )
                    guard result == 0 else {
                        throw loadedAPI.failure(
                            status: result,
                            runtime: createdRuntime,
                            appID: appID
                        )
                    }
                }

                let appID: Int32
                if let existingAppID = context.appIDs[gameID] {
                    let existingState = loadedAPI.midletState(
                        createdRuntime,
                        appID: existingAppID
                    )
                    if existingState == .destroyed || existingState == .error {
                        if existingState == .error {
                            // A failed visibility/input callback leaves its VM
                            // allocated until forced destruction. Starting a
                            // replacement without releasing that isolate first
                            // leaks scheduler/network/media state and can make
                            // the next launch fail with PHONEME_ERROR_SYSTEM_START
                            // (-6), especially on a physical device.
                            let destroyResult = loadedAPI.destroyMidlet(
                                createdRuntime,
                                appID: existingAppID
                            )
                            guard destroyResult == 0 else {
                                throw loadedAPI.failure(
                                    status: destroyResult,
                                    runtime: createdRuntime,
                                    appID: existingAppID
                                )
                            }
                        }
                        context.removeApplication(gameID: gameID)
                        guard let replacementAppID = context.nextAvailableAppID() else {
                            throw PhoneMECoreError.tooManyApplications
                        }
                        appID = replacementAppID
                        context.appIDs[gameID] = replacementAppID
                        context.gameIDsByAppID[replacementAppID] = gameID
                        try configureFramePacing(replacementAppID)
                        try configureHeap(replacementAppID)
                        let result = loadedAPI.startMidlet(
                            createdRuntime,
                            suiteID: resolvedSuiteID,
                            mainClass: mainClass,
                            appID: replacementAppID,
                            screenWidth: screenWidth,
                            screenHeight: screenHeight
                        )
                        guard result == 0 else {
                            let error = loadedAPI.failure(
                                status: result,
                                runtime: createdRuntime,
                                appID: replacementAppID
                            )
                            context.removeApplication(gameID: gameID)
                            throw error
                        }
                    } else {
                        appID = existingAppID
                        try configureFramePacing(existingAppID)
                        guard try Self.resumeApplicationIfNeeded(
                            api: loadedAPI,
                            runtime: createdRuntime,
                            appID: existingAppID,
                            token: currentLaunchToken
                        ) else {
                            return
                        }
                        guard try Self.restoreForegroundApplication(
                            api: loadedAPI,
                            runtime: createdRuntime,
                            appID: existingAppID,
                            screenWidth: screenWidth,
                            screenHeight: screenHeight,
                            token: currentLaunchToken
                        ) else {
                            return
                        }
                    }
                } else {
                    guard let newAppID = context.nextAvailableAppID() else {
                        throw PhoneMECoreError.tooManyApplications
                    }
                    appID = newAppID
                    context.appIDs[gameID] = newAppID
                    context.gameIDsByAppID[newAppID] = gameID
                    try configureFramePacing(newAppID)
                    try configureHeap(newAppID)
                    let result = loadedAPI.startMidlet(
                        createdRuntime,
                        suiteID: resolvedSuiteID,
                        mainClass: mainClass,
                        appID: newAppID,
                        screenWidth: screenWidth,
                        screenHeight: screenHeight
                    )
                    phoneMEMultitaskingLogger.info(
                        "Started game \(gameID.uuidString, privacy: .public): app=\(newAppID), suite=\(resolvedSuiteID), status=\(result)"
                    )
                    guard result == 0 else {
                        let error = loadedAPI.failure(
                            status: result,
                            runtime: createdRuntime,
                            appID: newAppID
                        )
                        context.removeApplication(gameID: gameID)
                        throw error
                    }
                }

                guard try Self.waitForForegroundApplication(
                    api: loadedAPI,
                    runtime: createdRuntime,
                    appID: appID,
                    screenWidth: screenWidth,
                    screenHeight: screenHeight,
                    token: currentLaunchToken
                ) else {
                    return
                }

                Self.configureMediaMetadata(
                    title: mediaTitle,
                    artist: mediaArtist,
                    artworkPath: mediaArtworkPath
                )

                DispatchQueue.main.async {
                    guard
                        let self,
                        self.launchIdentifier == currentLaunchIdentifier,
                        self.launchToken === currentLaunchToken,
                        !currentLaunchToken.isCancelled
                    else {
                        return
                    }
                    self.api = loadedAPI
                    self.runtime = createdRuntime
                    self.applyCurrentThermalPressure()
                    self.foregroundGameID = gameID
                    self.foregroundAppID = appID
                    self.pendingForegroundGameID = nil
                    self.runtimeIsSuspended = false
                    self.runtimeSuspensionRequested = false
                    self.setState(.running)
                    self.onApplicationStateChange?(gameID, .active)
                    self.onForegroundApplicationChange?(gameID)
                    self.startPolling(
                        api: loadedAPI,
                        runtime: createdRuntime,
                        launchIdentifier: currentLaunchIdentifier,
                        gameID: gameID,
                        appID: appID
                    )
                    if !self.shouldRunInForeground {
                        self.enterBackground()
                    }
                }
            } catch {
                phoneMEMultitaskingLogger.error(
                    "Launch failed for \(gameID.uuidString, privacy: .public): \(error.localizedDescription, privacy: .public)"
                )
                DispatchQueue.main.async { [weak self] in
                    guard
                        let self,
                        self.launchIdentifier == currentLaunchIdentifier,
                        self.launchToken === currentLaunchToken
                    else {
                        return
                    }
                    self.pendingForegroundGameID = nil
                    self.setState(.failed(error.localizedDescription))
                }
            }
        }
    }

    func configureJIT(
        enabled: Bool,
        completion: @escaping (Result<PhoneMECAPI.JITStatus, Error>) -> Void
    ) {
        let context = runtimeContext
        let fallbackAPI = api
        let fallbackRuntime = runtime

        runtimeQueue.async {
            let activeAPI = context.api ?? fallbackAPI
            let activeRuntime = context.runtime ?? fallbackRuntime
            let result: Result<PhoneMECAPI.JITStatus, Error>

            if let activeAPI, let activeRuntime {
                let status = activeAPI.configureJIT(
                    activeRuntime,
                    enabled: enabled
                )
                result = status == 0
                    ? .success(PhoneMECAPI.jitStatus)
                    : .failure(activeAPI.failure(
                        status: status,
                        runtime: activeRuntime
                    ))
            } else {
                // No VM exists yet. The preference is persisted by the caller
                // and createRuntime() applies it to every subsequently created
                // application VM.
                result = .success(PhoneMECAPI.jitStatus)
            }

            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    func refreshJITStatus(
        completion: @escaping (PhoneMECAPI.JITStatus) -> Void
    ) {
        // Re-enabling the current preference is intentional: the Core probes
        // executable memory again and activates already-created VMs after an
        // external JIT provider attaches to this process.
        configureJIT(enabled: PhoneMECAPI.jitEnabledByDefault) { result in
            completion((try? result.get()) ?? PhoneMECAPI.jitStatus)
        }
    }

    func setApplicationTranslation(
        gameID: UUID,
        enabled: Bool,
        provider: TranslationProvider,
        sourceLanguage: TranslationSourceLanguage,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let context = runtimeContext
        runtimeQueue.async {
            let result: Result<Void, Error>
            if let loadedAPI = context.api,
               let createdRuntime = context.runtime,
               let appID = context.appIDs[gameID] {
                let status = loadedAPI.configureApplicationTranslation(
                    createdRuntime,
                    appID: appID,
                    enabled: enabled,
                    provider: provider,
                    sourceLanguage: sourceLanguage
                )
                result = status == 0
                    ? .success(())
                    : .failure(loadedAPI.failure(
                        status: status,
                        runtime: createdRuntime,
                        appID: appID
                    ))
            } else {
                // The per-game preference is still persisted by the caller and
                // will be applied when this MIDlet starts next time.
                result = .success(())
            }
            DispatchQueue.main.async {
                completion(result)
            }
        }
    }

    func setApplicationFramePacing(
        gameID: UUID,
        framesPerSecond: Int,
        mode: GameProfile.FramePacingMode,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        let requestedFPS = GameProfile.resolvedFrameRate(
            framesPerSecond,
            mode: mode
        )
        let context = runtimeContext
        runtimeQueue.async { [weak self] in
            let result: Result<Void, Error>
            if let loadedAPI = context.api,
               let createdRuntime = context.runtime,
               let appID = context.appIDs[gameID] {
                let status = loadedAPI.configureApplicationFramePacing(
                    createdRuntime,
                    appID: appID,
                    framesPerSecond: requestedFPS,
                    mode: mode
                )
                result = status == 0
                    ? .success(())
                    : .failure(loadedAPI.failure(
                        status: status,
                        runtime: createdRuntime,
                        appID: appID
                    ))
            } else {
                // The profile is still persisted by the caller and is applied
                // automatically when the MIDlet starts next time.
                result = .success(())
            }

            DispatchQueue.main.async {
                guard let self else {
                    completion(result)
                    return
                }
                if case .success = result,
                   self.foregroundGameID == gameID,
                   let api = self.api,
                   let runtime = self.runtime,
                   let appID = self.foregroundAppID {
                    self.framePollingFramesPerSecond = requestedFPS
                    self.fpsFrameCount = 0
                    self.fpsMeasurementStartNanoseconds = DispatchTime.now()
                        .uptimeNanoseconds
                    // Invalidate callbacks from the previous cadence. Without
                    // a new token, a cancelled timer callback could reschedule
                    // the replacement timer using its old PollContext.
                    let pollingLaunchIdentifier = UUID()
                    self.launchIdentifier = pollingLaunchIdentifier
                    self.renderRequestBuffer = RenderRequestBuffer()
                    self.startPolling(
                        api: api,
                        runtime: runtime,
                        launchIdentifier: pollingLaunchIdentifier,
                        gameID: gameID,
                        appID: appID
                    )
                    if !self.shouldRunInForeground {
                        self.enterBackground()
                    }
                }
                completion(result)
            }
        }
    }

    func hideCurrentApplication() {
        guard let gameID = foregroundGameID ?? pendingForegroundGameID else {
            return
        }

        launchToken.cancel()
        launchIdentifier = UUID()
        stopForegroundPolling()
        foregroundGameID = nil
        foregroundAppID = nil
        pendingForegroundGameID = nil
        onApplicationStateChange?(gameID, .active)
        onForegroundApplicationChange?(nil)

        let context = runtimeContext
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async {
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }
            guard
                let api = context.api,
                let runtime = context.runtime
            else {
                return
            }
            let detached = Self.detachForegroundApplication(
                api: api,
                runtime: runtime
            )
            if !detached {
                phoneMEMultitaskingLogger.error(
                    "Failed to detach the foreground MIDlet after hiding its UI"
                )
            }
        }
    }

    func terminateApplication(gameID: UUID) {
        let context = runtimeContext
        let wasForeground = foregroundGameID == gameID
        let wasPending = pendingForegroundGameID == gameID
        let wasVisible = wasForeground || wasPending
        if wasPending {
            launchToken.cancel()
        }
        if wasVisible {
            launchIdentifier = UUID()
            stopForegroundPolling()
            foregroundGameID = nil
            foregroundAppID = nil
            pendingForegroundGameID = nil
            onForegroundApplicationChange?(nil)
        }

        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async { [weak self] in
            if wasVisible {
                inputQueue.sync { }
                pollQueue.sync { }
                renderQueue.sync { }
            }
            guard
                let loadedAPI = context.api,
                let createdRuntime = context.runtime,
                let appID = context.appIDs[gameID]
            else {
                return
            }

            _ = loadedAPI.destroyMidlet(createdRuntime, appID: appID)
            context.removeApplication(gameID: gameID)
            let shouldShutdown = context.appIDs.isEmpty
            if shouldShutdown {
                loadedAPI.stop(createdRuntime)
                loadedAPI.destroyRuntime(createdRuntime)
                context.reset()
            }

            DispatchQueue.main.async {
                guard let self else { return }
                self.onApplicationStateChange?(gameID, .destroyed)
                if wasVisible {
                    self.setState(.stopped)
                }
                if shouldShutdown {
                    self.api = nil
                    self.runtime = nil
                    self.runtimeIsSuspended = false
                    self.runtimeSuspensionRequested = false
                }
            }
        }
    }

    nonisolated private static func ensureRuntime(
        in context: RuntimeContext
    ) throws -> (PhoneMECAPI, PhoneMECAPI.RuntimeHandle) {
        if let api = context.api, let runtime = context.runtime {
            return (api, runtime)
        }
        let api = try PhoneMECAPI.load()
        guard let runtime = api.createRuntime() else {
            throw PhoneMECoreError.runtimeCreationFailed
        }
        context.api = api
        context.runtime = runtime
        return (api, runtime)
    }

    nonisolated private static func resolveSuiteID(
        gameID: UUID,
        jarURL: URL,
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        context: RuntimeContext
    ) throws -> Int32 {
        if let suiteID = context.suiteIDs[gameID] {
            return suiteID
        }

        let install = api.installJar(
            runtime,
            jarURL: jarURL,
            identityNamespace: gameID.uuidString.lowercased()
        )
        guard install.status == PHONEME_OK,
              let suiteID = install.suiteID else {
            throw api.failure(
                status: install.status,
                runtime: runtime
            )
        }
        context.suiteIDs[gameID] = suiteID
        return suiteID
    }

    nonisolated private static func resumeApplicationIfNeeded(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        appID: Int32,
        token: LaunchToken,
        timeout: TimeInterval = 2
    ) throws -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        var lastResult = Int32(PHONEME_OK)

        repeat {
            if token.isCancelled {
                return false
            }

            switch api.midletState(runtime, appID: appID) {
            case .active:
                return true
            case .paused:
                lastResult = api.resumeMidlet(runtime, appID: appID)
                if lastResult == PHONEME_OK {
                    return true
                }
                if lastResult != PHONEME_ERROR_SYSTEM_START {
                    throw api.failure(
                        status: lastResult,
                        runtime: runtime,
                        appID: appID
                    )
                }
            case .error:
                throw api.failure(
                    status: Int32(PHONEME_ERROR_SYSTEM_START),
                    runtime: runtime,
                    appID: appID
                )
            case .none, .destroyed:
                throw PhoneMECoreError.foregroundActivationFailed
            }

            Thread.sleep(forTimeInterval: 0.005)
        } while ProcessInfo.processInfo.systemUptime < deadline

        throw api.failure(
            status: lastResult,
            runtime: runtime,
            appID: appID
        )
    }

    nonisolated private static func restoreForegroundApplication(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        appID: Int32,
        screenWidth: Int,
        screenHeight: Int,
        token: LaunchToken,
        timeout: TimeInterval = 2
    ) throws -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        var lastResult = Int32(PHONEME_OK)

        repeat {
            if token.isCancelled {
                return false
            }

            let state = api.midletState(runtime, appID: appID)
            if state == .error {
                throw api.failure(
                    status: Int32(PHONEME_ERROR_SYSTEM_START),
                    runtime: runtime,
                    appID: appID
                )
            }
            if state == .none || state == .destroyed {
                throw PhoneMECoreError.foregroundActivationFailed
            }

            lastResult = api.setForeground(
                runtime,
                appID: appID,
                screenWidth: screenWidth,
                screenHeight: screenHeight
            )
            if lastResult == PHONEME_OK {
                return true
            }
            if lastResult != PHONEME_ERROR_SYSTEM_START {
                throw api.failure(
                    status: lastResult,
                    runtime: runtime,
                    appID: appID
                )
            }

            let updatedState = api.midletState(runtime, appID: appID)
            if updatedState == .error {
                throw api.failure(
                    status: Int32(PHONEME_ERROR_SYSTEM_START),
                    runtime: runtime,
                    appID: appID
                )
            }
            if updatedState == .none || updatedState == .destroyed {
                throw PhoneMECoreError.foregroundActivationFailed
            }

            // Error -6 also represents Core's transient invalid_state while a
            // lifecycle or visibility callback is still completing. Simulator
            // scheduling makes that window much easier to hit after Hide.
            Thread.sleep(forTimeInterval: 0.005)
        } while ProcessInfo.processInfo.systemUptime < deadline

        throw api.failure(
            status: lastResult,
            runtime: runtime,
            appID: appID
        )
    }

    nonisolated private static func waitForForegroundApplication(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        appID: Int32,
        screenWidth: Int,
        screenHeight: Int,
        token: LaunchToken
    ) throws -> Bool {
        var settleDeadline = ProcessInfo.processInfo.systemUptime + 0.25
        var didReassertForeground = false

        while ProcessInfo.processInfo.systemUptime < settleDeadline {
            if token.isCancelled {
                return false
            }

            let state = api.midletState(runtime, appID: appID)
            if state == .error {
                throw api.failure(
                    status: Int32(PHONEME_ERROR_SYSTEM_START),
                    runtime: runtime,
                    appID: appID
                )
            }
            if state == .destroyed {
                throw PhoneMECoreError.foregroundActivationFailed
            }

            // Runtime::start_midlet commits both ACTIVE and foreground_app_id
            // before returning. The old unconditional settle loop added 250 ms
            // to every normal launch even though polling can safely begin now.
            if state == .active, api.foregroundAppID(runtime) == appID {
                return true
            }

            // NAMS state/display callbacks are optional and arrive late on a
            // few phoneME builds. Re-assert only after the proxy is known to be
            // active; otherwise retain the original start request and finish a
            // short serialized settle instead of producing a false timeout.
            if state == .active,
               !didReassertForeground,
               api.foregroundAppID(runtime) != appID {
                guard try restoreForegroundApplication(
                    api: api,
                    runtime: runtime,
                    appID: appID,
                    screenWidth: screenWidth,
                    screenHeight: screenHeight,
                    token: token,
                    timeout: 0.5
                ) else {
                    return false
                }
                didReassertForeground = true
                if api.foregroundAppID(runtime) == appID {
                    return true
                }
                settleDeadline = ProcessInfo.processInfo.systemUptime + 0.05
            }

            Thread.sleep(forTimeInterval: 0.005)
        }
        return !token.isCancelled
    }

    nonisolated private static func detachForegroundApplication(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        timeout: TimeInterval = 2
    ) -> Bool {
        let deadline = ProcessInfo.processInfo.systemUptime + timeout
        repeat {
            let result = api.setForeground(
                runtime,
                appID: nil,
                screenWidth: 1,
                screenHeight: 1
            )
            if result == 0, api.foregroundAppID(runtime) == nil {
                return true
            }
            // Launch/lifecycle work may still hold the isolate briefly. Retry
            // the detach instead of silently leaving Core in foreground mode.
            Thread.sleep(forTimeInterval: 0.005)
        } while ProcessInfo.processInfo.systemUptime < deadline
        return api.foregroundAppID(runtime) == nil
    }

    nonisolated private static func configureMediaMetadata(
        title: String,
        artist: String,
        artworkPath: String?
    ) {
        let title = title.trimmingCharacters(in: .whitespacesAndNewlines)
        let artist = artist.trimmingCharacters(in: .whitespacesAndNewlines)
        title.withCString { titlePointer in
            artist.withCString { artistPointer in
                guard let artworkPath else {
                    phoneme_ios_media_set_application_metadata(
                        titlePointer,
                        artistPointer,
                        nil
                    )
                    return
                }
                artworkPath.withCString { artworkPointer in
                    phoneme_ios_media_set_application_metadata(
                        titlePointer,
                        artistPointer,
                        artworkPointer
                    )
                }
            }
        }
    }

    func stop(
        completion: (@MainActor @Sendable () -> Void)? = nil
    ) {
        let currentAPI = api
        let currentRuntime = runtime
        let needsResume = runtimeIsSuspended
            || runtimeSuspensionRequested
        launchToken.cancel()
        launchIdentifier = UUID()
        pendingForegroundGameID = nil
        clearCurrentRuntime()
        setState(.stopped)

        guard let currentAPI, let currentRuntime else {
            completion?()
            return
        }

        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async {
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }
            if needsResume {
                currentAPI.resume(currentRuntime)
            }
            currentAPI.stop(currentRuntime)
            currentAPI.destroyRuntime(currentRuntime)
            if let completion {
                DispatchQueue.main.async {
                    completion()
                }
            }
        }
    }

    func resetForStorageChange() {
        launchToken.cancel()
        launchIdentifier = UUID()
        pendingForegroundGameID = nil
        clearCurrentRuntime()

        let context = runtimeContext
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async { [weak self] in
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }

            if let loadedAPI = context.api,
               let createdRuntime = context.runtime {
                loadedAPI.stop(createdRuntime)
                loadedAPI.destroyRuntime(createdRuntime)
            }
            context.reset()

            DispatchQueue.main.async {
                self?.api = nil
                self?.runtime = nil
                self?.runtimeIsSuspended = false
                self?.runtimeSuspensionRequested = false
                self?.setState(.idle)
            }
        }
    }

    func enterBackground() {
        shouldRunInForeground = false

        // Suspend host presentation only, not the MIDlet lifecycle or VM.
        // Core keeps Java workers, timers, sockets, callSerially callbacks and
        // media running normally while Canvas/GameCanvas rendering is dropped.
        // No hideNotify() is exposed to the game. The optional iOS background
        // keeper is still required to stop iOS itself from suspending the whole
        // process after the app leaves the foreground.
        suspendRuntimeIfNeeded()
    }

    func enterForeground() {
        shouldRunInForeground = true
        if runtimeSuspensionRequested {
            resume()
        } else {
            resumeHostPollingAfterBackground()
        }
    }

    func suspend() {
        shouldRunInForeground = false
        suspendRuntimeIfNeeded()
    }

    private func suspendRuntimeIfNeeded() {
        guard
            !runtimeSuspensionRequested,
            let api,
            let runtime
        else {
            return
        }

        runtimeSuspensionRequested = true
        pauseHostPollingForBackground()

        let currentLaunchIdentifier = launchIdentifier
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async { [weak self] in
            // Stop all host-side C access before changing MIDP's global
            // suspend state. midp_suspend()/midp_resume() are not safe to race
            // framebuffer copies or Displayable event polling.
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }
            api.suspend(runtime)

            DispatchQueue.main.async {
                guard
                    let self,
                    self.launchIdentifier == currentLaunchIdentifier,
                    self.runtime == runtime
                else {
                    return
                }
                self.runtimeIsSuspended = true
            }
        }
    }

    func resume() {
        shouldRunInForeground = true
        guard
            runtimeSuspensionRequested,
            let api,
            let runtime
        else {
            resumeHostPollingAfterBackground()
            return
        }

        runtimeSuspensionRequested = false
        let currentLaunchIdentifier = launchIdentifier
        runtimeQueue.async { [weak self] in
            // This command is serialized behind any pending suspend command.
            api.resume(runtime)

            DispatchQueue.main.async {
                guard
                    let self,
                    self.launchIdentifier == currentLaunchIdentifier,
                    self.runtime == runtime
                else {
                    return
                }

                self.runtimeIsSuspended = false
                guard !self.runtimeSuspensionRequested else {
                    return
                }
                self.resumeHostPollingAfterBackground()
            }
        }
    }

    /// Persists deferred RMS (record store) writes before iOS can kill the
    /// process (memory warning). Runtime::suspend() already flushes on
    /// backgrounding; this covers the killed-while-active path.
    func flushRecordStores() {
        guard
            let api,
            let runtime
        else {
            return
        }
        // Serialized with destroyRuntime on runtimeQueue, so the handle stays
        // valid for the duration of the call.
        runtimeQueue.async {
            api.flushRMS(runtime)
        }
    }

    private func pauseHostPollingForBackground() {
        if let pollTimer, !pollTimerIsSuspended {
            pollTimer.suspend()
            pollTimerIsSuspended = true
        }
        fpsFrameCount = 0
        fpsMeasurementStartNanoseconds = DispatchTime.now()
            .uptimeNanoseconds
        onFPSChange?(0)
    }

    private func resumeHostPollingAfterBackground() {
        // The MIDlet may continue without repainting while iOS is backgrounded.
        // Force the first visible poll to publish the retained screen again.
        renderRequestBuffer.invalidateFrameGeneration()
        if let pollTimer, pollTimerIsSuspended {
            pollTimer.resume()
            pollTimerIsSuspended = false
        }
        fpsMeasurementStartNanoseconds = DispatchTime.now()
            .uptimeNanoseconds
    }

    private func wakeCanvasPollingForHostActivity() {
        activePollContextLock.lock()
        let context = activePollContext
        activePollContextLock.unlock()
        guard let context else { return }
        context.noteHostActivity()
        guard let pollTimer, !pollTimerIsSuspended else { return }
        // DispatchSourceTimer is Sendable and may be rescheduled directly.
        // Avoid a main -> pollQueue hop for every Core host-wake edge; the
        // source's handler still executes on pollQueue when the deadline fires.
        pollTimer.schedule(
            deadline: .now(),
            leeway: .milliseconds(1)
        )
    }

    func sendKey(_ key: J2MEKey, pressed: Bool) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        wakeCanvasPollingForHostActivity()
        inputQueue.async {
            api.sendKey(runtime, key: key, pressed: pressed)
        }
    }

    func sendPointer(x: Int32, y: Int32, action: Int32) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        wakeCanvasPollingForHostActivity()

        // Drag motion is a latest-value signal. Queueing every intermediate
        // coordinate can delay the eventual pointer-up and all LCDUI actions
        // behind it. Preserve down/up ordering, but coalesce action 3 moves.
        if action == 3 {
            scheduleContinuousInputFlush(
                if: continuousInputBuffer.submitPointerMove(x: x, y: y),
                api: api,
                runtime: runtime
            )
        } else {
            inputQueue.async {
                api.sendPointer(runtime, x: x, y: y, action: action)
            }
        }
    }

    func selectLCDUICommand(_ id: Int32) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        inputQueue.async {
            api.selectLCDUICommand(runtime, id: id)
        }
    }

    func selectLCDUIListItemCommand(
        componentID: Int32,
        index: Int,
        commandID: Int32
    ) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        inputQueue.async {
            api.selectLCDUIListItemCommand(
                runtime,
                componentID: componentID,
                index: index,
                commandID: commandID
            )
        }
    }

    func focusLCDUIItem(_ componentID: Int32) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        inputQueue.async {
            api.focusLCDUIItem(runtime, componentID: componentID)
        }
    }

    func activateLCDUIItem(_ componentID: Int32) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        inputQueue.async {
            api.activateLCDUIItem(runtime, componentID: componentID)
        }
    }

    func setLCDUIText(
        componentID: Int32,
        text: String,
        caretPosition: Int
    ) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        scheduleContinuousInputFlush(
            if: continuousInputBuffer.submitText(
                componentID: componentID,
                text: text,
                caretPosition: caretPosition
            ),
            api: api,
            runtime: runtime
        )
    }

    func setLCDUIChoice(
        componentID: Int32,
        index: Int,
        selected: Bool
    ) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        inputQueue.async {
            api.setLCDUIChoice(
                runtime,
                componentID: componentID,
                index: index,
                selected: selected
            )
        }
    }

    func setLCDUIGauge(componentID: Int32, value: Int) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        scheduleContinuousInputFlush(
            if: continuousInputBuffer.submitGauge(
                componentID: componentID,
                value: value
            ),
            api: api,
            runtime: runtime
        )
    }

    func setLCDUIDate(componentID: Int32, date: Date) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        scheduleContinuousInputFlush(
            if: continuousInputBuffer.submitDate(
                componentID: componentID,
                date: date
            ),
            api: api,
            runtime: runtime
        )
    }

    func setLCDUIScrollPosition(_ position: Int) {
        guard foregroundAppID != nil, let api, let runtime else { return }
        scheduleContinuousInputFlush(
            if: continuousInputBuffer.submitScrollPosition(position),
            api: api,
            runtime: runtime
        )
    }

    private func scheduleContinuousInputFlush(
        if shouldSchedule: Bool,
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle
    ) {
        guard shouldSchedule else { return }
        let buffer = continuousInputBuffer
        inputQueue.async {
            let snapshot = buffer.takeSnapshot()

            if let pointerMove = snapshot.pointerMove {
                api.sendPointer(
                    runtime,
                    x: pointerMove.x,
                    y: pointerMove.y,
                    action: 3
                )
            }
            for (componentID, update) in snapshot.textUpdates {
                api.setLCDUIText(
                    runtime,
                    componentID: componentID,
                    text: update.text,
                    caretPosition: update.caretPosition
                )
            }
            for (componentID, value) in snapshot.gaugeUpdates {
                api.setLCDUIGauge(
                    runtime,
                    componentID: componentID,
                    value: value
                )
            }
            for (componentID, date) in snapshot.dateUpdates {
                api.setLCDUIDate(
                    runtime,
                    componentID: componentID,
                    date: date
                )
            }
            if let position = snapshot.scrollPosition {
                api.setLCDUIScrollPosition(runtime, position: position)
            }
        }
    }

    private func startPolling(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        launchIdentifier: UUID,
        gameID: UUID? = nil,
        appID: Int32? = nil
    ) {
        // Harrier/libGDX-style requestRendering path: Core wakes this host
        // loop only when guest work can change what the user sees. The timer
        // below remains a watchdog/pacing source, not the sole producer of
        // presentation work.
        api.configureHostWake(runtime) { [weak self] in
            DispatchQueue.main.async { [weak self] in
                self?.wakeCanvasPollingForHostActivity()
            }
        }
        if let pollTimer {
            if pollTimerIsSuspended {
                pollTimer.resume()
                pollTimerIsSuspended = false
            }
            pollTimer.cancel()
        }

        let context = PollContext(
            framesPerSecond: framePollingFramesPerSecond,
            thermalPressure: PhoneMECAPI.currentThermalPressure
        )
        activePollContextLock.lock()
        activePollContext = context
        activePollContextLock.unlock()
        let renderBuffer = renderRequestBuffer
        let renderQueue = renderQueue
        let timer = DispatchSource.makeTimerSource(queue: pollQueue)

        // Use one monotonic deadline per poll instead of a repeating source.
        // Delayed callbacks are re-anchored one frame ahead and never replayed
        // in a burst, which keeps render cadence independent from Java time.
        timer.schedule(
            deadline: .now(),
            leeway: .nanoseconds(Int(context.pollLeewayNanoseconds))
        )
        timer.setEventHandler { [weak self] in
            guard !context.didReportRuntimeExit else { return }
            let pollNow = DispatchTime.now().uptimeNanoseconds
            if let pollMetrics = context.notePollForTelemetry(
                nowNanoseconds: pollNow
            ) {
                let presentation = api.presentationTelemetrySnapshot(reset: true)
                let wakeupsPerSecond = Double(pollMetrics.callbacks)
                    / max(pollMetrics.durationSeconds, 0.001)
                NSLog(
                    "[phoneME-thermal] thermal=%d target_fps=%llu polls_per_s=%.2f polls=%llu active=%llu idle_canvas=%llu idle_native=%llu frames=%llu uploads=%llu staged=%llu full=%llu regions=%llu uploaded_kb=%.1f acquire_ms=%.2f stage_ms=%.2f upload_ms=%.2f",
                    PhoneMECAPI.currentThermalPressure.rawValue,
                    pollMetrics.targetFramesPerSecond,
                    wakeupsPerSecond,
                    pollMetrics.callbacks,
                    pollMetrics.activeCallbacks,
                    pollMetrics.idleCanvasCallbacks,
                    pollMetrics.idleNativeCallbacks,
                    pollMetrics.framesProduced,
                    presentation.frames,
                    presentation.stagedFrames,
                    presentation.fullUploads,
                    presentation.regionUploads,
                    Double(presentation.uploadedBytes) / 1024.0,
                    presentation.acquireMilliseconds,
                    presentation.stagingMilliseconds,
                    presentation.uploadMilliseconds
                )
            }
            var shouldScheduleNextPoll = true
            defer {
                if shouldScheduleNextPoll {
                    self?.scheduleNextPoll(
                        context: context,
                        launchIdentifier: launchIdentifier
                    )
                }
            }

            guard api.isRunning(runtime) else {
                shouldScheduleNextPoll = false
                context.didReportRuntimeExit = true
                let exitCode = api.lastExitCode(runtime)
                let errorMessage = api.lastErrorMessage(runtime)
                DispatchQueue.main.async {
                    guard
                        let self,
                        self.launchIdentifier == launchIdentifier,
                        self.runtime == runtime
                    else {
                        return
                    }
                    self.finishRuntime(
                        api: api,
                        runtime: runtime,
                        exitCode: exitCode,
                        errorMessage: errorMessage
                    )
                }
                return
            }

            if let gameID, let appID {
                let applicationState = api.midletState(runtime, appID: appID)
                if applicationState == .destroyed || applicationState == .error {
                    shouldScheduleNextPoll = false
                    context.didReportRuntimeExit = true
                    let errorMessage = applicationState == .error
                        ? api.midletErrorMessage(runtime, appID: appID)
                            ?? api.lastErrorMessage(runtime)
                        : nil
                    DispatchQueue.main.async {
                        guard
                            let self,
                            self.launchIdentifier == launchIdentifier,
                            self.runtime == runtime
                        else {
                            return
                        }
                        self.finishApplication(
                            gameID: gameID,
                            state: applicationState,
                            errorMessage: errorMessage
                        )
                    }
                    return
                }
            }

            if context.visibleScreen?.usesNativeLCDUI == true {
                // Native LCDUI screens do not request Canvas frames, so pump
                // the VM explicitly to advance Alert timeouts, callSerially,
                // repaint coalescing, and other LCDUI event-thread work.
                api.pumpEvents(runtime)
            }

            let lcdUIEvents = api.drainLCDUIEvents(
                runtime,
                maximumCount: 512
            )
            var resolvedVisibleScreen = context.visibleScreen
            var imageComponentIDs = Set<Int32>()

            for event in lcdUIEvents {
                switch event.kind {
                case 1:
                    resolvedVisibleScreen = nil

                case 4:
                    resolvedVisibleScreen = VisibleScreen(
                        id: event.componentID,
                        componentType: event.componentType
                    )

                case 5, 6:
                    if resolvedVisibleScreen?.id == event.componentID {
                        resolvedVisibleScreen = nil
                    }

                default:
                    break
                }

                if event.arguments.3 == -1004 || event.arguments.3 == -1009 {
                    imageComponentIDs.insert(event.componentID)
                } else if event.kind == 12, event.arguments.3 < 0 {
                    imageComponentIDs.insert(event.arguments.3)
                }
            }

            context.visibleScreen = resolvedVisibleScreen

            let isIdleNativeScreen =
                resolvedVisibleScreen?.usesNativeLCDUI == true
                && lcdUIEvents.isEmpty
            context.updateNativeIdle(isIdleNativeScreen)

            if !lcdUIEvents.isEmpty {
                DispatchQueue.main.async {
                    guard
                        let self,
                        self.launchIdentifier == launchIdentifier,
                        self.runtime == runtime
                    else {
                        return
                    }
                    self.onLCDUIEvents?(lcdUIEvents)
                }
            }

            // One host tick can request at most one frame. RenderRequestBuffer
            // may replace an obsolete pending copy with the newest generation,
            // but it never executes extra game updates to catch up.
            let wantsFrame = resolvedVisibleScreen?.usesNativeLCDUI != true

            guard !imageComponentIDs.isEmpty || wantsFrame else {
                return
            }

            let shouldStartRenderWorker = renderBuffer.submit(
                imageComponentIDs: imageComponentIDs,
                visibleScreen: resolvedVisibleScreen,
                wantsFrame: wantsFrame
            )
            guard shouldStartRenderWorker else { return }

            renderQueue.async { [weak self] in
                repeat {
                    let snapshot = renderBuffer.takeSnapshot()
                    var images: [Int32: CGImage] = [:]

                    for componentID in snapshot.imageComponentIDs {
                        if let value = api.copyLCDUIImage(
                            runtime,
                            componentID: componentID
                        ) {
                            images[componentID] = value.image
                        }
                    }

                    let frame = snapshot.wantsFrame
                        && snapshot.visibleScreen?.usesNativeLCDUI != true
                        ? api.presentationFrame(
                            runtime,
                            after: snapshot.previousFrameGeneration
                        )
                        : nil
                    if snapshot.wantsFrame,
                       snapshot.visibleScreen?.usesNativeLCDUI != true {
                        context.noteCanvasFrameOutcome(
                            producedFrame: frame != nil
                        )
                    }
                    if let frame {
                        renderBuffer.updateFrameGeneration(frame.generation)
                    }

                    if !images.isEmpty || frame != nil {
                        DispatchQueue.main.async {
                            guard
                                let self,
                                self.launchIdentifier == launchIdentifier,
                                self.runtime == runtime
                            else {
                                return
                            }
                            if !images.isEmpty {
                                self.onLCDUIImages?(images)
                            }
                            if let frame {
                                self.deliver(frame)
                            }
                        }
                    }
                } while renderBuffer.completeIteration()
            }
        }

        pollTimer = timer
        timer.resume()
    }

    private func scheduleNextPoll(
        context: PollContext,
        launchIdentifier: UUID
    ) {
        guard
            self.launchIdentifier == launchIdentifier,
            let pollTimer
        else {
            return
        }

        let nowNanoseconds = DispatchTime.now().uptimeNanoseconds
        pollTimer.schedule(
            deadline: context.nextPollDeadline(after: nowNanoseconds),
            leeway: .nanoseconds(Int(context.pollLeewayNanoseconds))
        )
    }

    private func deliver(_ frame: PhoneMEFrame) {
        onFrame?(frame)

        fpsFrameCount += 1
        let nowNanoseconds = DispatchTime.now().uptimeNanoseconds
        guard nowNanoseconds >= fpsMeasurementStartNanoseconds else {
            fpsFrameCount = 0
            fpsMeasurementStartNanoseconds = nowNanoseconds
            return
        }

        let elapsedNanoseconds = nowNanoseconds
            - fpsMeasurementStartNanoseconds
        if elapsedNanoseconds >= 500_000_000 {
            let measuredFPS = Double(fpsFrameCount) * 1_000_000_000
                / Double(elapsedNanoseconds)
            onFPSChange?(measuredFPS)
            fpsFrameCount = 0
            fpsMeasurementStartNanoseconds = nowNanoseconds
        }
    }

    private func finishApplication(
        gameID: UUID,
        state: PhoneMECAPI.AppState,
        errorMessage: String?
    ) {
        let context = runtimeContext
        launchToken.cancel()
        stopForegroundPolling()
        foregroundGameID = nil
        foregroundAppID = nil
        if pendingForegroundGameID == gameID {
            pendingForegroundGameID = nil
        }
        onApplicationStateChange?(gameID, state)
        onForegroundApplicationChange?(nil)
        setState(
            state == .error
                ? .failed(
                    errorMessage
                        ?? L10n.string(
                            "The J2ME application stopped unexpectedly."
                        )
                )
                : .stopped
        )

        // A MIDlet calling notifyDestroyed() must follow the same native
        // teardown path as the host Exit action. Otherwise the Swift session
        // disappears while its runtime, sockets, media and worker queues stay
        // alive in RuntimeContext.
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        runtimeQueue.async { [weak self] in
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }

            guard
                let loadedAPI = context.api,
                let createdRuntime = context.runtime
            else {
                context.removeApplication(gameID: gameID)
                return
            }

            if let appID = context.appIDs[gameID] {
                _ = loadedAPI.destroyMidlet(createdRuntime, appID: appID)
            }
            context.removeApplication(gameID: gameID)

            let shouldShutdown = context.appIDs.isEmpty
            if shouldShutdown {
                loadedAPI.stop(createdRuntime)
                loadedAPI.destroyRuntime(createdRuntime)
                context.reset()
            }

            guard shouldShutdown else { return }
            DispatchQueue.main.async {
                guard
                    let self,
                    self.runtime == createdRuntime
                else {
                    return
                }
                self.api = nil
                self.runtime = nil
                self.runtimeIsSuspended = false
                self.runtimeSuspensionRequested = false
            }
        }
    }

    private func finishRuntime(
        api: PhoneMECAPI,
        runtime: PhoneMECAPI.RuntimeHandle,
        exitCode: Int32,
        errorMessage: String?
    ) {
        guard self.runtime == runtime else {
            return
        }

        clearCurrentRuntime()
        let inputQueue = inputQueue
        let pollQueue = pollQueue
        let renderQueue = renderQueue
        let context = runtimeContext
        runtimeQueue.async {
            inputQueue.sync { }
            pollQueue.sync { }
            renderQueue.sync { }
            api.destroyRuntime(runtime)
            context.reset()
        }

        if exitCode < 0 {
            setState(.failed(
                errorMessage
                    ?? PhoneMECoreError.launchFailed(exitCode)
                        .localizedDescription
            ))
        } else {
            setState(.stopped)
        }
    }

    private func stopForegroundPolling() {
        if let pollTimer {
            if pollTimerIsSuspended {
                pollTimer.resume()
            }
            pollTimer.cancel()
        }
        pollTimer = nil
        pollTimerIsSuspended = false
        activePollContextLock.lock()
        activePollContext = nil
        activePollContextLock.unlock()
        continuousInputBuffer.reset()
        renderRequestBuffer = RenderRequestBuffer()
        fpsFrameCount = 0
        fpsMeasurementStartNanoseconds = DispatchTime.now()
            .uptimeNanoseconds
        onFPSChange?(0)
    }

    private func clearCurrentRuntime() {
        launchToken.cancel()
        pendingForegroundGameID = nil
        foregroundGameID = nil
        foregroundAppID = nil
        if let pollTimer {
            // Dispatch sources must be resumed before their final cancellation.
            if pollTimerIsSuspended {
                pollTimer.resume()
            }
            pollTimer.cancel()
        }
        pollTimer = nil
        pollTimerIsSuspended = false
        activePollContextLock.lock()
        activePollContext = nil
        activePollContextLock.unlock()
        runtimeIsSuspended = false
        runtimeSuspensionRequested = false
        continuousInputBuffer.reset()
        // Give the next launch a fresh coalescing state. Any old render worker
        // retains the previous buffer and is rejected by launchIdentifier.
        renderRequestBuffer = RenderRequestBuffer()
        fpsFrameCount = 0
        fpsMeasurementStartNanoseconds = DispatchTime.now()
            .uptimeNanoseconds
        onFPSChange?(0)
        runtime = nil
        api = nil
    }

    private func setState(_ state: EmulatorState) {
        onStateChange?(state)
    }
}
