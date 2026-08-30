import CoreGraphics
import Foundation
#if canImport(Metal)
import Metal
#endif

#if canImport(Metal)
private final class PhoneMEMetalFrameTexturePool: @unchecked Sendable {
    private struct TextureKey: Hashable {
        let width: Int
        let height: Int
        let pixelFormat: UInt
    }

    private struct TextureSlot {
        let texture: MTLTexture
        let generation: UInt64
        let epoch: UInt64
    }

    private let device: MTLDevice
    private let lock = NSLock()
    private let maximumRetainedTextures: Int
    private var available: [TextureKey: [TextureSlot]] = [:]
    private var retainedTextureCount = 0
    private var currentEpoch: UInt64 = 1

    init?(maximumRetainedTextures: Int) {
        guard let device = MTLCreateSystemDefaultDevice() else { return nil }
        self.device = device
        self.maximumRetainedTextures = max(maximumRetainedTextures, 2)
    }

    func acquire(
        width: Int,
        height: Int,
        pixelFormat: MTLPixelFormat
    ) -> PhoneMEMetalFrameTextureLease? {
        guard width > 0, height > 0 else { return nil }
        let key = TextureKey(
            width: width,
            height: height,
            pixelFormat: pixelFormat.rawValue
        )

        lock.lock()
        if var bucket = available[key], let slot = bucket.popLast() {
            if bucket.isEmpty {
                available.removeValue(forKey: key)
            } else {
                available[key] = bucket
            }
            retainedTextureCount -= 1
            let epoch = currentEpoch
            lock.unlock()
            return PhoneMEMetalFrameTextureLease(
                texture: slot.texture,
                width: width,
                height: height,
                contentGeneration: slot.epoch == epoch ? slot.generation : 0,
                epoch: epoch,
                pool: self
            )
        }
        let epoch = currentEpoch
        lock.unlock()

        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat,
            width: width,
            height: height,
            mipmapped: false
        )
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: descriptor) else {
            return nil
        }
        texture.label = "phoneME J2ME framebuffer \(width)x\(height)"
        return PhoneMEMetalFrameTextureLease(
            texture: texture,
            width: width,
            height: height,
            contentGeneration: 0,
            epoch: epoch,
            pool: self
        )
    }

    func invalidateContents() {
        lock.lock()
        currentEpoch &+= 1
        if currentEpoch == 0 { currentEpoch = 1 }
        let keys = Array(available.keys)
        for key in keys {
            available[key] = available[key]?.map { slot in
                TextureSlot(
                    texture: slot.texture,
                    generation: 0,
                    epoch: currentEpoch
                )
            }
        }
        lock.unlock()
    }

    fileprivate func recycle(
        texture: MTLTexture,
        width: Int,
        height: Int,
        generation: UInt64,
        epoch: UInt64
    ) {
        lock.lock()
        guard retainedTextureCount < maximumRetainedTextures else {
            lock.unlock()
            return
        }
        let key = TextureKey(
            width: width,
            height: height,
            pixelFormat: texture.pixelFormat.rawValue
        )
        available[key, default: []].append(TextureSlot(
            texture: texture,
            generation: epoch == currentEpoch ? generation : 0,
            epoch: currentEpoch
        ))
        retainedTextureCount += 1
        lock.unlock()
    }
}

private final class PhoneMEMetalFrameTextureLease: @unchecked Sendable {
    let texture: MTLTexture
    let width: Int
    let height: Int
    var contentGeneration: UInt64
    let epoch: UInt64
    private var pool: PhoneMEMetalFrameTexturePool?

    init(
        texture: MTLTexture,
        width: Int,
        height: Int,
        contentGeneration: UInt64,
        epoch: UInt64,
        pool: PhoneMEMetalFrameTexturePool
    ) {
        self.texture = texture
        self.width = width
        self.height = height
        self.contentGeneration = contentGeneration
        self.epoch = epoch
        self.pool = pool
    }

    deinit {
        pool?.recycle(
            texture: texture,
            width: width,
            height: height,
            generation: contentGeneration,
            epoch: epoch
        )
        pool = nil
    }
}

private struct PhoneMEFrameDamage: Sendable {
    let x: Int
    let y: Int
    let width: Int
    let height: Int

    var area: Int { width * height }

    func intersectsOrTouches(_ other: PhoneMEFrameDamage) -> Bool {
        x <= other.x + other.width
            && other.x <= x + width
            && y <= other.y + other.height
            && other.y <= y + height
    }

    func union(_ other: PhoneMEFrameDamage) -> PhoneMEFrameDamage {
        let left = min(x, other.x)
        let top = min(y, other.y)
        let right = max(x + width, other.x + other.width)
        let bottom = max(y + height, other.y + other.height)
        return PhoneMEFrameDamage(
            x: left,
            y: top,
            width: right - left,
            height: bottom - top
        )
    }
}

private enum PhoneMEMetalFrameUploadPlan {
    case full
    case regions([PhoneMEFrameDamage])
}

private final class PhoneMEMetalDamageHistory: @unchecked Sendable {
    private struct Record {
        let generation: UInt64
        let isFull: Bool
        let regions: [PhoneMEFrameDamage]
    }

    private let lock = NSLock()
    private let maximumRecords = 32
    private var records: [Record] = []

    func reset() {
        lock.lock()
        records.removeAll(keepingCapacity: true)
        lock.unlock()
    }

    func planAndRecord(
        textureGeneration: UInt64,
        previousPresentedGeneration: UInt64,
        currentGeneration: UInt64,
        currentRegions: [PhoneMEFrameDamage]?,
        width: Int,
        height: Int
    ) -> PhoneMEMetalFrameUploadPlan {
        lock.lock()
        defer { lock.unlock() }

        let generationsAreContiguous = previousPresentedGeneration != 0
            && previousPresentedGeneration != UInt64.max
            && currentGeneration == previousPresentedGeneration + 1
        let currentIsFull = !generationsAreContiguous || currentRegions == nil
        let safeCurrentRegions = currentIsFull ? [] : (currentRegions ?? [])
        records.append(Record(
            generation: currentGeneration,
            isFull: currentIsFull,
            regions: safeCurrentRegions
        ))
        if records.count > maximumRecords {
            records.removeFirst(records.count - maximumRecords)
        }

        guard
            currentGeneration != UInt64.max,
            textureGeneration > 0,
            textureGeneration < currentGeneration,
            width > 0,
            height > 0,
            textureGeneration != UInt64.max
        else {
            return .full
        }

        var expectedGeneration = textureGeneration + 1
        var accumulated: [PhoneMEFrameDamage] = []
        for record in records where record.generation > textureGeneration {
            guard record.generation <= currentGeneration else { break }
            guard record.generation == expectedGeneration, !record.isFull else {
                return .full
            }
            accumulated.append(contentsOf: record.regions)
            if record.generation == UInt64.max { return .full }
            expectedGeneration = record.generation + 1
        }
        guard expectedGeneration == currentGeneration + 1,
              !accumulated.isEmpty else {
            return .full
        }

        var merged: [PhoneMEFrameDamage] = []
        for region in accumulated {
            guard
                region.x >= 0,
                region.y >= 0,
                region.width > 0,
                region.height > 0,
                region.x + region.width <= width,
                region.y + region.height <= height
            else {
                return .full
            }

            var candidate = region
            var index = 0
            while index < merged.count {
                if candidate.intersectsOrTouches(merged[index]) {
                    candidate = candidate.union(merged.remove(at: index))
                    index = 0
                } else {
                    index += 1
                }
            }
            merged.append(candidate)
        }

        let frameArea = width * height
        let damagedArea = merged.reduce(0) { $0 + $1.area }
        guard merged.count <= 8,
              damagedArea * 2 < frameArea else {
            return .full
        }
        return .regions(merged)
    }
}
#endif

final class PhoneMEFrame: @unchecked Sendable {
    let width: Int
    let height: Int
    let generation: UInt64
    let image: CGImage?

#if canImport(Metal)
    private let metalLease: PhoneMEMetalFrameTextureLease?
    var metalTexture: MTLTexture? { metalLease?.texture }

    func retainMetalResources(until commandBuffer: MTLCommandBuffer) {
        guard let metalLease else { return }
        commandBuffer.addCompletedHandler { [metalLease] _ in
            _ = metalLease
        }
    }
#endif

    fileprivate init(
        width: Int,
        height: Int,
        generation: UInt64,
        image: CGImage?
    ) {
        self.width = width
        self.height = height
        self.generation = generation
        self.image = image
#if canImport(Metal)
        self.metalLease = nil
#endif
    }

#if canImport(Metal)
    fileprivate init(
        width: Int,
        height: Int,
        generation: UInt64,
        image: CGImage?,
        metalLease: PhoneMEMetalFrameTextureLease?
    ) {
        self.width = width
        self.height = height
        self.generation = generation
        self.image = image
        self.metalLease = metalLease
    }
#endif

    func makeCGImage() -> CGImage? {
        if let image { return image }
#if canImport(Metal)
        guard let texture = metalTexture, width > 0, height > 0 else {
            return nil
        }
        let byteCount = width * height * 4
        var bytes = Data(count: byteCount)
        bytes.withUnsafeMutableBytes { storage in
            guard let baseAddress = storage.baseAddress else { return }
            texture.getBytes(
                baseAddress,
                bytesPerRow: width * 4,
                from: MTLRegionMake2D(0, 0, width, height),
                mipmapLevel: 0
            )
        }
        guard let provider = CGDataProvider(data: bytes as CFData) else {
            return nil
        }
        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(
                rawValue: CGImageAlphaInfo.noneSkipLast.rawValue
            ),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
#else
        return nil
#endif
    }
}

private final class PhoneMEPixelBufferPool: @unchecked Sendable {
    private struct Buffer {
        let pointer: UnsafeMutableRawPointer
        let capacity: Int
    }

    private let lock = NSLock()
    private let maximumRetainedBuffers: Int
    private var available: [Buffer] = []

    init(maximumRetainedBuffers: Int) {
        self.maximumRetainedBuffers = max(maximumRetainedBuffers, 1)
    }

    func acquire(minimumCapacity: Int) -> PhoneMEPixelBufferLease {
        let requiredCapacity = max(minimumCapacity, 1)
        lock.lock()
        var bestIndex: Int?
        for index in available.indices
            where available[index].capacity >= requiredCapacity {
            guard let currentBest = bestIndex else {
                bestIndex = index
                continue
            }
            if available[index].capacity < available[currentBest].capacity {
                bestIndex = index
            }
        }

        if let bestIndex {
            let buffer = available.remove(at: bestIndex)
            lock.unlock()
            return PhoneMEPixelBufferLease(
                pointer: buffer.pointer,
                capacity: buffer.capacity,
                pool: self
            )
        }
        lock.unlock()

        let roundedCapacity = (requiredCapacity + 4_095) & ~4_095
        return PhoneMEPixelBufferLease(
            pointer: UnsafeMutableRawPointer.allocate(
                byteCount: roundedCapacity,
                alignment: 64
            ),
            capacity: roundedCapacity,
            pool: self
        )
    }

    fileprivate func recycle(
        pointer: UnsafeMutableRawPointer,
        capacity: Int
    ) {
        lock.lock()
        if available.count < maximumRetainedBuffers {
            available.append(Buffer(pointer: pointer, capacity: capacity))
            lock.unlock()
        } else {
            lock.unlock()
            pointer.deallocate()
        }
    }

    deinit {
        for buffer in available {
            buffer.pointer.deallocate()
        }
    }
}

private final class PhoneMEPixelBufferLease: @unchecked Sendable {
    let pointer: UnsafeMutableRawPointer
    let capacity: Int
    private var pool: PhoneMEPixelBufferPool?

    init(
        pointer: UnsafeMutableRawPointer,
        capacity: Int,
        pool: PhoneMEPixelBufferPool
    ) {
        self.pointer = pointer
        self.capacity = capacity
        self.pool = pool
    }

    deinit {
        pool?.recycle(pointer: pointer, capacity: capacity)
        pool = nil
    }
}

private let phoneMEPixelBufferRelease: CGDataProviderReleaseDataCallback = {
    info,
    _,
    _ in
    guard let info else { return }
    Unmanaged<PhoneMEPixelBufferLease>.fromOpaque(info).release()
}

final class PhoneMECAPI: @unchecked Sendable {
    struct RuntimeHandle: @unchecked Sendable, Equatable {
        fileprivate let rawValue: UnsafeMutableRawPointer
    }

    enum AppState: Int32, Sendable {
        case none = 0
        case active = 1
        case paused = 2
        case destroyed = 3
        case error = 4
    }

    enum JITStatus: Int32, Sendable {
        case unavailable = 0
        case ready = 1
    }

    enum ThermalPressure: Int32, Sendable {
        case nominal = 0
        case fair = 1
        case serious = 2
        case critical = 3
    }

    static var currentThermalPressure: ThermalPressure {
        switch ProcessInfo.processInfo.thermalState {
        case .nominal:
            return .nominal
        case .fair:
            return .fair
        case .serious:
            return .serious
        case .critical:
            return .critical
        @unknown default:
            return .serious
        }
    }

    static let trollStoreBuildInfoKey = "PhoneMETrollStoreJIT"

    static var jitStatus: JITStatus {
        JITStatus(rawValue: phoneme_jit_status()) ?? .unavailable
    }

    static var jitEnabledByDefault: Bool {
#if PHONEME_INTERPRETER_ONLY
        false
#else
        true
#endif
    }

    static var isTrollStoreJITBuild: Bool {
        Bundle.main.object(forInfoDictionaryKey: trollStoreBuildInfoKey)
            as? Bool ?? false
    }

    static var trollStoreJITURL: URL? {
        guard let bundleIdentifier = Bundle.main.bundleIdentifier else {
            return nil
        }
        var components = URLComponents()
        components.scheme = "apple-magnifier"
        components.host = "enable-jit"
        components.queryItems = [
            URLQueryItem(name: "bundle-id", value: bundleIdentifier)
        ]
        return components.url
    }

    enum PushBackgroundPolicy: Int32, Sendable {
        case foregroundOnly = 0
        case systemManaged = 1
    }

    enum PushRequestKind: Int32, Sendable {
        case connection = 1
        case alarm = 2
    }

    struct PushLaunchRequest: Sendable {
        let requestID: UInt64
        let kind: PushRequestKind
        let createdAtMillis: Int64
        let target: String
        let midlet: String
    }

    struct LCDUIEvent: Sendable {
        let kind: Int32
        let componentID: Int32
        let parentID: Int32
        let componentType: Int32
        let index: Int32
        let arguments: (Int32, Int32, Int32, Int32)
        let value64: Int64
        let generation: UInt64
        let text: String
        let detail: String
    }

    private static let rgbColorSpace = CGColorSpaceCreateDeviceRGB()

    private let layout: PhoneMERuntimeLayout
    private let frameBufferPool = PhoneMEPixelBufferPool(
        maximumRetainedBuffers: 3
    )
    private let imageBufferPool = PhoneMEPixelBufferPool(
        maximumRetainedBuffers: 12
    )
#if canImport(Metal)
    private let metalFrameTexturePool = PhoneMEMetalFrameTexturePool(
        maximumRetainedTextures: 4
    )
    private let metalDamageHistory = PhoneMEMetalDamageHistory()
#endif

    private init(layout: PhoneMERuntimeLayout) {
        self.layout = layout
    }

    static func load() throws -> PhoneMECAPI {
        PhoneMECAPI(layout: try PhoneMERuntimeResources.prepare())
    }

    static func load(gameID: UUID) throws -> PhoneMECAPI {
        PhoneMECAPI(layout: try PhoneMERuntimeResources.prepare(for: gameID))
    }

    func createRuntime() -> RuntimeHandle? {
#if canImport(Metal)
        metalFrameTexturePool?.invalidateContents()
        metalDamageHistory.reset()
#endif
        guard let rawRuntime = phoneme_create() else {
            NSLog("[phoneMECore] phoneme_create failed home=%@", layout.homeURL.path)
            return nil
        }

        let handle = RuntimeHandle(rawValue: rawRuntime)
        let result = layout.homeURL.path.withCString { homePath in
            phoneme_configure(rawRuntime, homePath, nil)
        }

        guard result == 0 else {
            let detail = lastErrorMessage(handle) ?? "unknown configure error"
            NSLog(
                "[phoneMECore] phoneme_configure failed status=%d home=%@ detail=%@",
                result,
                layout.homeURL.path,
                detail
            )
            phoneme_destroy(rawRuntime)
            return nil
        }

        let thermalResult = phoneme_set_thermal_pressure(
            rawRuntime,
            Self.currentThermalPressure.rawValue
        )
        guard thermalResult == 0 else {
            let detail = lastErrorMessage(handle) ?? "unknown thermal configure error"
            NSLog(
                "[phoneMECore] phoneme_set_thermal_pressure failed status=%d detail=%@",
                thermalResult,
                detail
            )
            phoneme_destroy(rawRuntime)
            return nil
        }

        let jitResult = phoneme_configure_jit(
            rawRuntime,
            Self.jitEnabledByDefault ? 1 : 0
        )
        guard jitResult == 0 else {
            let detail = lastErrorMessage(handle) ?? "unknown JIT configure error"
            NSLog(
                "[phoneMECore] phoneme_configure_jit failed status=%d home=%@ detail=%@",
                jitResult,
                layout.homeURL.path,
                detail
            )
            phoneme_destroy(rawRuntime)
            return nil
        }
        return handle
    }

    func destroyRuntime(_ runtime: RuntimeHandle?) {
        phoneme_destroy(runtime?.rawValue)
    }

    func configureKeymap(
        _ runtime: RuntimeHandle?,
        up: Int32,
        down: Int32,
        left: Int32,
        right: Int32,
        fire: Int32,
        softLeft: Int32,
        softRight: Int32
    ) -> Int32 {
        phoneme_configure_keymap(
            runtime?.rawValue,
            up,
            down,
            left,
            right,
            fire,
            softLeft,
            softRight
        )
    }

    func configureApplicationFramePacing(
        _ runtime: RuntimeHandle?,
        appID: Int32,
        framesPerSecond: Int,
        mode: GameProfile.FramePacingMode
    ) -> Int32 {
        phoneme_configure_app_frame_pacing(
            runtime?.rawValue,
            appID,
            Int32(clamping: framesPerSecond),
            mode.coreValue
        )
    }

    func configureApplicationHeap(
        _ runtime: RuntimeHandle?,
        appID: Int32,
        heapMegabytes: Int
    ) -> Int32 {
        phoneme_configure_app_heap(
            runtime?.rawValue,
            appID,
            Int32(clamping: heapMegabytes)
        )
    }

    func configureJIT(
        _ runtime: RuntimeHandle?,
        enabled: Bool
    ) -> Int32 {
        phoneme_configure_jit(runtime?.rawValue, enabled ? 1 : 0)
    }

    func setThermalPressure(
        _ runtime: RuntimeHandle?,
        pressure: ThermalPressure
    ) -> Int32 {
        phoneme_set_thermal_pressure(runtime?.rawValue, pressure.rawValue)
    }

    func configureTranslation(
        _ runtime: RuntimeHandle?,
        enabled: Bool,
        provider: TranslationProvider,
        sourceLanguage: TranslationSourceLanguage
    ) -> Int32 {
        sourceLanguage.rawValue.withCString { sourceLanguage in
            "vi".withCString { targetLanguage in
                phoneme_configure_translation_v2(
                    runtime?.rawValue,
                    enabled ? 1 : 0,
                    provider.coreValue,
                    sourceLanguage,
                    targetLanguage
                )
            }
        }
    }

    func configureApplicationTranslation(
        _ runtime: RuntimeHandle?,
        appID: Int32,
        enabled: Bool,
        provider: TranslationProvider,
        sourceLanguage: TranslationSourceLanguage
    ) -> Int32 {
        sourceLanguage.rawValue.withCString { sourceLanguage in
            "vi".withCString { targetLanguage in
                phoneme_configure_app_translation_v2(
                    runtime?.rawValue,
                    appID,
                    enabled ? 1 : 0,
                    provider.coreValue,
                    sourceLanguage,
                    targetLanguage
                )
            }
        }
    }

    func installJar(
        _ runtime: RuntimeHandle?,
        jarURL: URL,
        identityNamespace: String? = nil
    ) -> (status: Int32, suiteID: Int32?) {
        var suiteID: Int32 = 0
        let status = jarURL.path.withCString { path in
            guard let identityNamespace else {
                return phoneme_install_jar(
                    runtime?.rawValue,
                    path,
                    &suiteID
                )
            }
            return identityNamespace.withCString { scope in
                phoneme_install_jar_scoped(
                    runtime?.rawValue,
                    path,
                    scope,
                    &suiteID
                )
            }
        }
        return (status, status == 0 ? suiteID : nil)
    }

    func findInstalledSuite(
        _ runtime: RuntimeHandle?,
        vendor: String,
        name: String,
        version: String,
        identityNamespace: String? = nil
    ) -> Int32? {
        var suiteID: Int32 = 0
        let status = vendor.withCString { vendorPointer in
            name.withCString { namePointer in
                version.withCString { versionPointer in
                    guard let identityNamespace else {
                        return phoneme_find_installed_suite(
                            runtime?.rawValue,
                            vendorPointer,
                            namePointer,
                            versionPointer,
                            &suiteID
                        )
                    }
                    return identityNamespace.withCString { scope in
                        phoneme_find_installed_suite_scoped(
                            runtime?.rawValue,
                            vendorPointer,
                            namePointer,
                            versionPointer,
                            scope,
                            &suiteID
                        )
                    }
                }
            }
        }
        guard status == PHONEME_OK, suiteID > 0 else { return nil }
        return suiteID
    }

    func uninstallSuite(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        removeData: Bool
    ) -> Int32 {
        phoneme_uninstall_suite(
            runtime?.rawValue,
            suiteID,
            removeData ? 1 : 0
        )
    }

    func setSuiteTrusted(
        _ runtime: RuntimeHandle?,
        suiteID: Int32
    ) -> Int32 {
        phoneme_set_suite_trust(
            runtime?.rawValue,
            suiteID,
            Int32(PHONEME_SUITE_TRUSTED.rawValue)
        )
    }

    func lastInstallStage() -> Int32 {
        phoneme_last_install_stage()
    }

    func lastSuiteStoreStage() -> Int32 {
        phoneme_last_suite_store_stage()
    }

    func startSystem(_ runtime: RuntimeHandle?) -> Int32 {
        phoneme_start_system(runtime?.rawValue)
    }

    func startMidlet(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        mainClass: String,
        appID: Int32,
        screenWidth: Int,
        screenHeight: Int
    ) -> Int32 {
        mainClass.withCString { className in
            phoneme_start_midlet(
                runtime?.rawValue,
                suiteID,
                className,
                appID,
                Int32(clamping: screenWidth),
                Int32(clamping: screenHeight)
            )
        }
    }

    func setForeground(
        _ runtime: RuntimeHandle?,
        appID: Int32?,
        screenWidth: Int,
        screenHeight: Int
    ) -> Int32 {
        phoneme_set_foreground(
            runtime?.rawValue,
            appID ?? 0,
            Int32(clamping: screenWidth),
            Int32(clamping: screenHeight)
        )
    }

    func pauseMidlet(_ runtime: RuntimeHandle?, appID: Int32) -> Int32 {
        phoneme_pause_midlet(runtime?.rawValue, appID)
    }

    func resumeMidlet(_ runtime: RuntimeHandle?, appID: Int32) -> Int32 {
        phoneme_resume_midlet(runtime?.rawValue, appID)
    }

    func destroyMidlet(_ runtime: RuntimeHandle?, appID: Int32) -> Int32 {
        phoneme_destroy_midlet(runtime?.rawValue, appID)
    }

    func setPushBackgroundPolicy(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        policy: PushBackgroundPolicy
    ) -> Int32 {
        phoneme_push_set_background_policy(
            runtime?.rawValue,
            suiteID,
            policy.rawValue
        )
    }

    func notifyPushConnectionAvailable(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        connection: String,
        sourceAddress: String? = nil,
        receivedAtMillis: Int64
    ) -> Int32 {
        connection.withCString { connectionValue in
            guard let sourceAddress else {
                return phoneme_push_notify_connection_available(
                    runtime?.rawValue,
                    suiteID,
                    connectionValue,
                    receivedAtMillis
                )
            }
            return sourceAddress.withCString { sourceValue in
                phoneme_push_notify_connection_available_from_source(
                    runtime?.rawValue,
                    suiteID,
                    connectionValue,
                    sourceValue,
                    receivedAtMillis
                )
            }
        }
    }

    func pollPushLaunchRequests(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        nowMillis: Int64,
        backgroundExecutionGranted: Bool,
        maximumCount: Int = 32
    ) -> [PushLaunchRequest] {
        guard let runtime, maximumCount > 0 else { return [] }
        var rawRequests = Array(
            repeating: PhoneMEPushLaunchRequest(),
            count: maximumCount
        )
        let copied = rawRequests.withUnsafeMutableBufferPointer { buffer in
            phoneme_push_poll_launch_requests(
                runtime.rawValue,
                suiteID,
                nowMillis,
                backgroundExecutionGranted ? 1 : 0,
                buffer.baseAddress,
                Int32(clamping: buffer.count)
            )
        }
        guard copied > 0 else { return [] }
        return rawRequests.prefix(Int(copied)).compactMap { raw in
            guard let kind = PushRequestKind(rawValue: raw.kind) else {
                return nil
            }
            var target = raw.target
            var midlet = raw.midlet
            return PushLaunchRequest(
                requestID: raw.request_id,
                kind: kind,
                createdAtMillis: raw.created_at_millis,
                target: Self.string(from: &target),
                midlet: Self.string(from: &midlet)
            )
        }
    }

    func acknowledgePushLaunchRequest(
        _ runtime: RuntimeHandle?,
        suiteID: Int32,
        requestID: UInt64
    ) -> Int32 {
        phoneme_push_acknowledge_launch_request(
            runtime?.rawValue,
            suiteID,
            requestID
        )
    }

    func midletState(
        _ runtime: RuntimeHandle?,
        appID: Int32
    ) -> AppState {
        AppState(rawValue: phoneme_midlet_state(runtime?.rawValue, appID))
            ?? .none
    }

    func foregroundAppID(_ runtime: RuntimeHandle?) -> Int32? {
        let appID = phoneme_foreground_app_id(runtime?.rawValue)
        return appID > 0 ? appID : nil
    }

    func startJar(
        _ runtime: RuntimeHandle?,
        jarURL: URL,
        mainClass: String,
        screenWidth: Int,
        screenHeight: Int
    ) -> Int32 {
        jarURL.path.withCString { path in
            mainClass.withCString { className in
                phoneme_start_jar(
                    runtime?.rawValue,
                    path,
                    className,
                    Int32(clamping: screenWidth),
                    Int32(clamping: screenHeight)
                )
            }
        }
    }

    func stop(_ runtime: RuntimeHandle?) {
        phoneme_stop(runtime?.rawValue)
    }

    func suspend(_ runtime: RuntimeHandle?) {
        phoneme_suspend(runtime?.rawValue)
    }

    func resume(_ runtime: RuntimeHandle?) {
        phoneme_resume(runtime?.rawValue)
    }

    func flushRMS(_ runtime: RuntimeHandle?) {
        phoneme_flush_rms(runtime?.rawValue)
    }

    func isRunning(_ runtime: RuntimeHandle?) -> Bool {
        phoneme_is_running(runtime?.rawValue) != 0
    }

    func lastExitCode(_ runtime: RuntimeHandle?) -> Int32 {
        phoneme_last_exit_code(runtime?.rawValue)
    }

    func lastErrorMessage(_ runtime: RuntimeHandle?) -> String? {
        Self.copyDiagnostic { destination, capacity in
            phoneme_copy_last_error_message(
                runtime?.rawValue,
                destination,
                capacity
            )
        }
    }

    func midletErrorMessage(
        _ runtime: RuntimeHandle?,
        appID: Int32
    ) -> String? {
        Self.copyDiagnostic { destination, capacity in
            phoneme_copy_midlet_error_message(
                runtime?.rawValue,
                appID,
                destination,
                capacity
            )
        }
    }

    func failure(
        status: Int32,
        runtime: RuntimeHandle?,
        appID: Int32? = nil
    ) -> PhoneMECoreError {
        let message = appID.flatMap {
            midletErrorMessage(runtime, appID: $0)
        } ?? lastErrorMessage(runtime)
        guard let message, !message.isEmpty else {
            return .launchFailed(status)
        }
        return .runtimeFailure(code: status, message: message)
    }

    func sendKey(_ runtime: RuntimeHandle?, key: J2MEKey, pressed: Bool) {
        phoneme_send_key(runtime?.rawValue, key.rawValue, pressed ? 1 : 0)
    }

    func sendPointer(
        _ runtime: RuntimeHandle?,
        x: Int32,
        y: Int32,
        action: Int32
    ) {
        phoneme_send_pointer(runtime?.rawValue, x, y, action)
    }

    func pumpEvents(_ runtime: RuntimeHandle?) {
        phoneme_pump_events(runtime?.rawValue)
    }

    func drainLCDUIEvents(
        _ runtime: RuntimeHandle?,
        maximumCount: Int = 512
    ) -> [LCDUIEvent] {
        guard let runtime, maximumCount > 0 else { return [] }

        var events: [LCDUIEvent] = []
        events.reserveCapacity(min(maximumCount, 64))
        var rawEvent = PhoneMELCDUIEvent()
        while events.count < maximumCount,
              phoneme_poll_lcdui_event(runtime.rawValue, &rawEvent) != 0 {
            var text = rawEvent.text
            var detail = rawEvent.detail
            events.append(
                LCDUIEvent(
                    kind: rawEvent.kind,
                    componentID: rawEvent.component_id,
                    parentID: rawEvent.parent_id,
                    componentType: rawEvent.component_type,
                    index: rawEvent.index,
                    arguments: (
                        rawEvent.arg0,
                        rawEvent.arg1,
                        rawEvent.arg2,
                        rawEvent.arg3
                    ),
                    value64: rawEvent.value64,
                    generation: rawEvent.generation,
                    text: Self.string(from: &text),
                    detail: Self.string(from: &detail)
                )
            )
            rawEvent = PhoneMELCDUIEvent()
        }
        return events
    }

    func selectLCDUICommand(_ runtime: RuntimeHandle?, id: Int32) {
        phoneme_lcdui_select_command(runtime?.rawValue, id)
    }

    func selectLCDUIListItemCommand(
        _ runtime: RuntimeHandle?,
        componentID: Int32,
        index: Int,
        commandID: Int32
    ) {
        phoneme_lcdui_select_list_item_command(
            runtime?.rawValue,
            componentID,
            Int32(clamping: index),
            commandID
        )
    }

    func focusLCDUIItem(_ runtime: RuntimeHandle?, componentID: Int32) {
        phoneme_lcdui_focus_item(runtime?.rawValue, componentID)
    }

    func activateLCDUIItem(_ runtime: RuntimeHandle?, componentID: Int32) {
        phoneme_lcdui_activate_item(runtime?.rawValue, componentID)
    }

    func setLCDUIText(
        _ runtime: RuntimeHandle?,
        componentID: Int32,
        text: String,
        caretPosition: Int
    ) {
        text.withCString { value in
            phoneme_lcdui_set_text(
                runtime?.rawValue,
                componentID,
                value,
                Int32(clamping: caretPosition)
            )
        }
    }

    func setLCDUIChoice(
        _ runtime: RuntimeHandle?,
        componentID: Int32,
        index: Int,
        selected: Bool
    ) {
        phoneme_lcdui_set_choice(
            runtime?.rawValue,
            componentID,
            Int32(clamping: index),
            selected ? 1 : 0
        )
    }

    func setLCDUIGauge(
        _ runtime: RuntimeHandle?,
        componentID: Int32,
        value: Int
    ) {
        phoneme_lcdui_set_gauge(
            runtime?.rawValue,
            componentID,
            Int32(clamping: value)
        )
    }

    func setLCDUIDate(
        _ runtime: RuntimeHandle?,
        componentID: Int32,
        date: Date
    ) {
        phoneme_lcdui_set_date(
            runtime?.rawValue,
            componentID,
            Int64(date.timeIntervalSince1970)
        )
    }

    func setLCDUIScrollPosition(
        _ runtime: RuntimeHandle?,
        position: Int
    ) {
        phoneme_lcdui_set_scroll_position(
            runtime?.rawValue,
            Int32(clamping: position)
        )
    }

    private static func string<T>(from tuple: inout T) -> String {
        withUnsafePointer(to: &tuple) { pointer in
            pointer.withMemoryRebound(
                to: CChar.self,
                capacity: 768
            ) { characters in
                String(cString: characters)
            }
        }
    }

    func copyLCDUIImage(
        _ runtime: RuntimeHandle?,
        componentID: Int32
    ) -> (image: CGImage, generation: UInt64)? {
        var width: Int32 = 0
        var height: Int32 = 0
        var generation: UInt64 = 0
        let requiredByteCount = phoneme_copy_lcdui_image_rgba(
            runtime?.rawValue,
            componentID,
            nil,
            0,
            &width,
            &height,
            &generation
        )

        guard requiredByteCount > 0, width > 0, height > 0 else {
            return nil
        }

        let lease = imageBufferPool.acquire(
            minimumCapacity: Int(requiredByteCount)
        )
        let writtenByteCount = phoneme_copy_lcdui_image_rgba(
            runtime?.rawValue,
            componentID,
            lease.pointer.assumingMemoryBound(to: UInt8.self),
            Int32(clamping: lease.capacity),
            &width,
            &height,
            &generation
        )

        guard
            writtenByteCount == requiredByteCount,
            let image = Self.makeImage(
                lease: lease,
                byteCount: Int(writtenByteCount),
                width: Int(width),
                height: Int(height),
                shouldInterpolate: true,
                isOpaque: false
            )
        else {
            return nil
        }
        return (image, generation)
    }

    func copyFrame(
        _ runtime: RuntimeHandle?,
        after previousGeneration: UInt64
    ) -> (image: CGImage, generation: UInt64)? {
        var width: Int32 = 0
        var height: Int32 = 0
        var generation: UInt64 = 0
        let requiredByteCount = phoneme_copy_frame_rgba(
            runtime?.rawValue,
            nil,
            0,
            &width,
            &height,
            &generation
        )

        guard
            requiredByteCount > 0,
            width > 0,
            height > 0,
            generation != previousGeneration
        else {
            return nil
        }

        // The provider retains this lease until Core Graphics releases the
        // image. Returned storage is recycled instead of allocating a fresh
        // Data object for every changed framebuffer generation.
        let lease = frameBufferPool.acquire(
            minimumCapacity: Int(requiredByteCount)
        )
        let writtenByteCount = phoneme_copy_frame_rgba(
            runtime?.rawValue,
            lease.pointer.assumingMemoryBound(to: UInt8.self),
            Int32(clamping: lease.capacity),
            &width,
            &height,
            &generation
        )

        guard
            writtenByteCount == requiredByteCount,
            Int(width) * Int(height) * 4 <= Int(writtenByteCount),
            let image = Self.makeImage(
                lease: lease,
                byteCount: Int(writtenByteCount),
                width: Int(width),
                height: Int(height),
                shouldInterpolate: false,
                isOpaque: true
            )
        else {
            return nil
        }

        return (image, generation)
    }

    func presentationFrame(
        _ runtime: RuntimeHandle?,
        after previousGeneration: UInt64
    ) -> PhoneMEFrame? {
#if canImport(UIKit) && canImport(Metal)
        guard let runtime, let metalFrameTexturePool else {
            if let copied = copyFrame(runtime, after: previousGeneration) {
                return PhoneMEFrame(
                    width: copied.image.width,
                    height: copied.image.height,
                    generation: copied.generation,
                    image: copied.image,
                    metalLease: nil
                )
            }
            return nil
        }

        var width: Int32 = 0
        var height: Int32 = 0
        var generation: UInt64 = 0
        var pixelFormat: Int32 = 0
        var damageCount: Int32 = 0
        let maximumDamageRegions = 32
        var currentDamageRegions: [PhoneMEFrameDamage]?
        let pixels = withUnsafeTemporaryAllocation(
            of: PhoneMEFrameDamageRegion.self,
            capacity: maximumDamageRegions
        ) { storage in
            let acquired = phoneme_acquire_current_frame_native_regions_since(
                runtime.rawValue,
                previousGeneration,
                &width,
                &height,
                &generation,
                &pixelFormat,
                storage.baseAddress,
                Int32(storage.count),
                &damageCount
            )
            guard damageCount >= 0,
                  damageCount <= Int32(storage.count) else {
                currentDamageRegions = nil
                return acquired
            }
            var regions: [PhoneMEFrameDamage] = []
            regions.reserveCapacity(Int(damageCount))
            for index in 0..<Int(damageCount) {
                let region = storage[index]
                regions.append(PhoneMEFrameDamage(
                    x: Int(region.x),
                    y: Int(region.y),
                    width: Int(region.width),
                    height: Int(region.height)
                ))
            }
            currentDamageRegions = regions
            return acquired
        }
        guard let pixels else {
            return nil
        }

        let frameWidth = Int(width)
        let frameHeight = Int(height)
        let metalPixelFormat: MTLPixelFormat
        switch pixelFormat {
        case Int32(PHONEME_FRAME_PIXEL_BGRA8.rawValue):
            metalPixelFormat = .bgra8Unorm
        case Int32(PHONEME_FRAME_PIXEL_RGBA8.rawValue):
            metalPixelFormat = .rgba8Unorm
        default:
            phoneme_release_frame_rgba(runtime.rawValue)
            return nil
        }
        guard
            width > 0,
            height > 0,
            generation != previousGeneration,
            let lease = metalFrameTexturePool.acquire(
                width: frameWidth,
                height: frameHeight,
                pixelFormat: metalPixelFormat
            )
        else {
            return nil
        }

        let uploadPlan = metalDamageHistory.planAndRecord(
            textureGeneration: lease.contentGeneration,
            previousPresentedGeneration: previousGeneration,
            currentGeneration: generation,
            currentRegions: currentDamageRegions,
            width: frameWidth,
            height: frameHeight
        )
        let bytesPerRow = frameWidth * 4

        // The Core framebuffer lease holds its producer mutex. For larger
        // games, keeping that lock across MTLTexture.replace() can stall the
        // Java paint thread behind the host upload and show up as a periodic
        // hitch. Stage only large frames, and only the bytes that Metal will
        // actually upload. Small classic 240x320 frames keep the zero-copy path
        // to preserve the low-power behavior.
        let shouldStageBeforeMetalUpload = frameWidth * frameHeight >= 128 * 1024
        var stagedPixels: PhoneMEPixelBufferLease?
        var framebufferLeaseReleased = false
        var uploadPixels = UnsafeRawPointer(pixels)
        if shouldStageBeforeMetalUpload {
            let stage = frameBufferPool.acquire(
                minimumCapacity: frameWidth * frameHeight * 4
            )
            switch uploadPlan {
            case .full:
                memcpy(
                    stage.pointer,
                    pixels,
                    frameWidth * frameHeight * 4
                )
            case .regions(let regions):
                for region in regions {
                    let rowBytes = region.width * 4
                    for row in 0..<region.height {
                        let byteOffset = (
                            (region.y + row) * frameWidth + region.x
                        ) * 4
                        memcpy(
                            stage.pointer.advanced(by: byteOffset),
                            pixels.advanced(by: byteOffset),
                            rowBytes
                        )
                    }
                }
            }
            phoneme_release_frame_rgba(runtime.rawValue)
            framebufferLeaseReleased = true
            stagedPixels = stage
            uploadPixels = UnsafeRawPointer(stage.pointer)
        }
        defer {
            _ = stagedPixels
            if !framebufferLeaseReleased {
                phoneme_release_frame_rgba(runtime.rawValue)
            }
        }

        switch uploadPlan {
        case .full:
            lease.texture.replace(
                region: MTLRegionMake2D(0, 0, frameWidth, frameHeight),
                mipmapLevel: 0,
                withBytes: uploadPixels,
                bytesPerRow: bytesPerRow
            )
        case .regions(let regions):
            for region in regions {
                let byteOffset = (region.y * frameWidth + region.x) * 4
                lease.texture.replace(
                    region: MTLRegionMake2D(
                        region.x,
                        region.y,
                        region.width,
                        region.height
                    ),
                    mipmapLevel: 0,
                    withBytes: uploadPixels.advanced(by: byteOffset),
                    bytesPerRow: bytesPerRow
                )
            }
        }
        lease.contentGeneration = generation
        return PhoneMEFrame(
            width: frameWidth,
            height: frameHeight,
            generation: generation,
            image: nil,
            metalLease: lease
        )
#else
        guard let copied = copyFrame(runtime, after: previousGeneration) else {
            return nil
        }
        return PhoneMEFrame(
            width: copied.image.width,
            height: copied.image.height,
            generation: copied.generation,
            image: copied.image
        )
#endif
    }

    private static func copyDiagnostic(
        _ copy: (UnsafeMutablePointer<CChar>?, Int32) -> Int32
    ) -> String? {
        let requiredCount = copy(nil, 0)
        guard requiredCount > 0 else { return nil }

        var buffer = Array<CChar>(
            repeating: 0,
            count: Int(requiredCount) + 1
        )
        let copiedCount = buffer.withUnsafeMutableBufferPointer { storage in
            copy(storage.baseAddress, Int32(clamping: storage.count))
        }
        guard copiedCount >= 0 else { return nil }
        return buffer.withUnsafeBufferPointer { storage in
            guard let baseAddress = storage.baseAddress else { return nil }
            return String(validatingCString: baseAddress)
        }
    }

    private static func makeImage(
        lease: PhoneMEPixelBufferLease,
        byteCount: Int,
        width: Int,
        height: Int,
        shouldInterpolate: Bool,
        isOpaque: Bool
    ) -> CGImage? {
        guard
            width > 0,
            height > 0,
            byteCount > 0,
            width * height * 4 <= byteCount,
            byteCount <= lease.capacity
        else {
            return nil
        }

        let retainedLease = Unmanaged.passRetained(lease).toOpaque()
        guard let provider = CGDataProvider(
            dataInfo: retainedLease,
            data: lease.pointer,
            size: byteCount,
            releaseData: phoneMEPixelBufferRelease
        ) else {
            Unmanaged<PhoneMEPixelBufferLease>
                .fromOpaque(retainedLease)
                .release()
            return nil
        }
        return CGImage(
            width: width,
            height: height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: width * 4,
            space: rgbColorSpace,
            bitmapInfo: CGBitmapInfo(
                rawValue: (
                    isOpaque
                        ? CGImageAlphaInfo.noneSkipLast
                        : CGImageAlphaInfo.last
                ).rawValue
            ),
            provider: provider,
            decode: nil,
            shouldInterpolate: shouldInterpolate,
            intent: .defaultIntent
        )
    }
}

enum PhoneMECoreError: LocalizedError {
    case runtimeNotLinked
    case runtimeResourcesMissing
    case runtimeCreationFailed
    case mainClassMissing
    case applicationNotPrepared
    case tooManyApplications
    case foregroundActivationFailed
    case launchFailed(Int32)
    case runtimeFailure(code: Int32, message: String)

    var errorDescription: String? {
        switch self {
        case .runtimeNotLinked:
            return L10n.string("Runtime is not available.")
        case .runtimeResourcesMissing:
            return L10n.string(
                "The bundled phoneME runtime resources are missing."
            )
        case .runtimeCreationFailed:
            return L10n.string("Failed to create runtime.")
        case .mainClassMissing:
            return L10n.string(
                "The JAR manifest does not contain a valid MIDlet-1 class."
            )
        case .applicationNotPrepared:
            return L10n.string(
                "This application was imported after multitasking started. Close the running applications once so it can be prepared."
            )
        case .tooManyApplications:
            return L10n.string(
                "The maximum of 64 simultaneous J2ME applications has been reached."
            )
        case .foregroundActivationFailed:
            return L10n.string(
                "The J2ME application could not become active in the foreground."
            )
        case let .launchFailed(code):
            return L10n.format("Failed to start application (error %d).", code)
        case let .runtimeFailure(_, message):
            return message
        }
    }
}
