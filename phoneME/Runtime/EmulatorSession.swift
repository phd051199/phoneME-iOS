import CoreGraphics
import Foundation

@MainActor
final class EmulatorFrameStore: ObservableObject {
    @Published private(set) var frameSize: CGSize?
    private(set) var frame: PhoneMEFrame?
    private var observers: [UUID: (PhoneMEFrame?) -> Void] = [:]

    @discardableResult
    func observeFrames(
        _ observer: @escaping (PhoneMEFrame?) -> Void
    ) -> UUID {
        let identifier = UUID()
        observers[identifier] = observer
        observer(frame)
        return identifier
    }

    func removeFrameObserver(_ identifier: UUID) {
        observers.removeValue(forKey: identifier)
    }

    fileprivate func update(_ frame: PhoneMEFrame) {
        self.frame = frame
        let nextSize = CGSize(width: frame.width, height: frame.height)
        if frameSize != nextSize {
            frameSize = nextSize
        }
        for observer in observers.values {
            observer(frame)
        }
    }

    fileprivate func reset() {
        frame = nil
        if frameSize != nil {
            frameSize = nil
        }
        for observer in observers.values {
            observer(nil)
        }
    }
}

@MainActor
final class EmulatorFPSStore: ObservableObject {
    @Published private(set) var value: Double = 0

    fileprivate func update(_ value: Double) {
        guard self.value != value else { return }
        self.value = value
    }

    fileprivate func reset() {
        update(0)
    }
}

@MainActor
final class LCDUIImageSlot: ObservableObject {
    @Published private(set) var image: CGImage?

    fileprivate init(image: CGImage? = nil) {
        self.image = image
    }

    fileprivate func update(_ image: CGImage?) {
        if let current = self.image, let image, current === image {
            return
        }
        if self.image == nil, image == nil {
            return
        }
        self.image = image
    }
}

@MainActor
final class LCDUIImageStore: ObservableObject {
    private(set) var images: [Int32: CGImage] = [:]
    private var slots: [Int32: LCDUIImageSlot] = [:]

    subscript(componentID: Int32) -> CGImage? {
        images[componentID]
    }

    func slot(for componentID: Int32) -> LCDUIImageSlot {
        if let slot = slots[componentID] {
            return slot
        }
        let slot = LCDUIImageSlot(image: images[componentID])
        slots[componentID] = slot
        return slot
    }

    fileprivate func merge(_ updates: [Int32: CGImage]) {
        for (componentID, image) in updates {
            if let current = images[componentID], current === image {
                continue
            }
            images[componentID] = image
            slots[componentID]?.update(image)
        }
    }

    fileprivate func replace(with nextImages: [Int32: CGImage]) {
        for componentID in images.keys where nextImages[componentID] == nil {
            slots[componentID]?.update(nil)
        }
        for (componentID, image) in nextImages {
            if let current = images[componentID], current === image {
                continue
            }
            slots[componentID]?.update(image)
        }
        images = nextImages
    }

    fileprivate func reset() {
        images.removeAll(keepingCapacity: true)
        for slot in slots.values {
            slot.update(nil)
        }
        slots.removeAll(keepingCapacity: true)
    }
}

enum EmulatorPresentationMode: Equatable, Sendable {
    case framebuffer
    case nativeLCDUI
}

struct RunningJ2MEApplication: Identifiable, Equatable {
    enum State: Equatable {
        case starting
        case foreground
        case background
        case paused
        case failed
    }

    let game: Game
    var state: State

    var id: UUID { game.id }
}

private struct EmulatorPresentationSnapshot {
    let frame: PhoneMEFrame?
    let presentationMode: EmulatorPresentationMode
    let lcdUI: LCDUIState
    let lcdUIImages: [Int32: CGImage]
}

@MainActor
final class EmulatorSession: ObservableObject {
    @Published private(set) var state: EmulatorState = .idle
    @Published private(set) var presentationMode: EmulatorPresentationMode = .framebuffer
    @Published private(set) var lcdUI: LCDUIState = .empty
    @Published private(set) var currentGame: Game?
    @Published private(set) var runningApplications: [UUID: RunningJ2MEApplication] = [:]
    @Published private(set) var jitStatus = PhoneMECAPI.jitStatus

    let frameStore = EmulatorFrameStore()
    let fpsStore = EmulatorFPSStore()
    let lcdUIImageStore = LCDUIImageStore()
    var frame: PhoneMEFrame? { frameStore.frame }
    var lcdUIImages: [Int32: CGImage] { lcdUIImageStore.images }

    var isPresentingNativeLCDUI: Bool {
        presentationMode == .nativeLCDUI && lcdUI.hasNativeScreen
    }

    private let engine: EmbeddedPhoneMEEngine
    private var presentationSnapshots: [UUID: EmulatorPresentationSnapshot] = [:]

    init(engine: EmbeddedPhoneMEEngine? = nil) {
        let resolvedEngine = engine ?? EmbeddedPhoneMEEngine()
        self.engine = resolvedEngine

        resolvedEngine.onFrame = { [weak self] frame in
            // Framebuffer generations may continue briefly while phoneME is
            // switching Displayables. Only SCREEN_SHOWN is authoritative;
            // otherwise a stale Canvas refresh can cover a native Form/List.
            self?.frameStore.update(frame)
        }
        resolvedEngine.onLCDUIEvents = { [weak self] events in
            guard let self else { return }
            var nextState = self.lcdUI
            var nextImages = self.lcdUIImageStore.images
            var imagesChanged = false
            var nextPresentationMode = self.presentationMode
            for event in events {
                nextState.apply(event)
                if event.arguments.3 == -1004 {
                    imagesChanged = nextImages.removeValue(
                        forKey: event.componentID
                    ) != nil || imagesChanged
                } else if event.kind == 12, event.arguments.3 < 0 {
                    imagesChanged = nextImages.removeValue(
                        forKey: event.arguments.3
                    ) != nil || imagesChanged
                }
                switch event.kind {
                case 1:
                    imagesChanged = !nextImages.isEmpty || imagesChanged
                    nextImages.removeAll(keepingCapacity: true)
                    nextPresentationMode = .framebuffer

                case 4:
                    nextPresentationMode = event.componentType
                        == LCDUIState.ComponentType.canvas.rawValue
                        ? .framebuffer
                        : .nativeLCDUI

                case 5, 6:
                    if !nextState.hasNativeScreen && !nextState.isCanvasVisible {
                        nextPresentationMode = .framebuffer
                    }
                    let filteredImages = nextImages.filter {
                        nextState.items[$0.key] != nil
                    }
                    imagesChanged = filteredImages.count != nextImages.count
                        || imagesChanged
                    nextImages = filteredImages

                case 11:
                    imagesChanged = nextImages.removeValue(
                        forKey: event.componentID
                    ) != nil || imagesChanged
                    let filteredImages = nextImages.filter {
                        !Self.isChoiceImageKey(
                            $0.key,
                            componentID: event.componentID
                        )
                    }
                    imagesChanged = filteredImages.count != nextImages.count
                        || imagesChanged
                    nextImages = filteredImages

                case 13:
                    let filteredImages = nextImages.filter {
                        !Self.isChoiceImageKey(
                            $0.key,
                            componentID: event.componentID
                        )
                    }
                    imagesChanged = filteredImages.count != nextImages.count
                        || imagesChanged
                    nextImages = filteredImages

                default:
                    break
                }
            }

            if nextState.hasNativeScreen {
                nextPresentationMode = .nativeLCDUI
            } else if nextState.isCanvasVisible {
                nextPresentationMode = .framebuffer
            } else if nextPresentationMode == .nativeLCDUI {
                nextPresentationMode = .framebuffer
            }

            if nextState != self.lcdUI {
                self.lcdUI = nextState
            }
            if imagesChanged {
                self.lcdUIImageStore.replace(with: nextImages)
            }
            if nextPresentationMode != self.presentationMode {
                self.presentationMode = nextPresentationMode
            }
        }
        resolvedEngine.onLCDUIImages = { [weak self] images in
            guard let self, !images.isEmpty else { return }
            self.lcdUIImageStore.merge(images)
        }
        resolvedEngine.onStateChange = { [weak self] state in
            guard let self else { return }
            self.state = state
            switch state {
            case .stopped:
                self.resetSessionResources(clearCurrentGame: true)
            case .failed:
                if let currentGame,
                   var application = self.runningApplications[currentGame.id] {
                    application.state = .failed
                    self.runningApplications[currentGame.id] = application
                }
                self.resetSessionResources(clearCurrentGame: false)
            default:
                break
            }
        }
        resolvedEngine.onFPSChange = { [weak self] value in
            self?.fpsStore.update(value)
        }
        resolvedEngine.onApplicationStateChange = { [weak self] gameID, state in
            guard let self else { return }
            switch state {
            case .destroyed:
                self.presentationSnapshots.removeValue(forKey: gameID)
                self.runningApplications.removeValue(forKey: gameID)
                if self.currentGame?.id == gameID {
                    self.resetSessionResources(clearCurrentGame: true)
                }

            case .error:
                self.presentationSnapshots.removeValue(forKey: gameID)
                if var application = self.runningApplications[gameID] {
                    application.state = .failed
                    self.runningApplications[gameID] = application
                }

            case .paused:
                if var application = self.runningApplications[gameID] {
                    application.state = .paused
                    self.runningApplications[gameID] = application
                }

            case .active:
                if var application = self.runningApplications[gameID] {
                    application.state = self.currentGame?.id == gameID
                        ? .foreground
                        : .background
                    self.runningApplications[gameID] = application
                }

            case .none:
                break
            }
        }
        resolvedEngine.onForegroundApplicationChange = { [weak self] gameID in
            guard let self else { return }
            for id in Array(self.runningApplications.keys) {
                guard var application = self.runningApplications[id] else {
                    continue
                }
                if id == gameID {
                    application.state = .foreground
                } else {
                    switch application.state {
                    case .starting, .foreground, .background:
                        application.state = .background
                    case .paused, .failed:
                        break
                    }
                }
                self.runningApplications[id] = application
            }
        }
    }

    func prepareApplications(
        _ games: [Game],
        fileURL: (Game) -> URL
    ) {
        let applications = Dictionary(
            uniqueKeysWithValues: games.map { game in
                (game.id, fileURL(game))
            }
        )
        engine.prepareApplications(applications)
    }

    func exportRMS(
        for game: Game,
        jarURL: URL,
        completion: @escaping (Result<Data, Error>) -> Void
    ) {
        engine.exportRMS(
            gameID: game.id,
            jarURL: jarURL,
            completion: completion
        )
    }

    func importRMS(
        from sourceURL: URL,
        for game: Game,
        jarURL: URL,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        engine.importRMS(
            from: sourceURL,
            gameID: game.id,
            jarURL: jarURL,
            completion: completion
        )
    }

    func uninstallApplication(
        _ game: Game,
        jarURL: URL,
        removeData: Bool,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        engine.uninstallApplication(
            gameID: game.id,
            jarURL: jarURL,
            removeData: removeData,
            completion: completion
        )
    }

    func launch(
        game: Game,
        jarURL: URL,
        artworkURL: URL?,
        profile: GameProfile
    ) {
        let profile = profile.normalized()
        let resumesExistingApplication: Bool
        if let existingApplication = runningApplications[game.id] {
            switch existingApplication.state {
            case .starting, .foreground, .background, .paused:
                resumesExistingApplication = true
            case .failed:
                resumesExistingApplication = false
            }
        } else {
            resumesExistingApplication = false
        }

        currentGame = game
        runningApplications[game.id] = RunningJ2MEApplication(
            game: game,
            state: .starting
        )
        fpsStore.reset()
        if !resumesExistingApplication ||
            !restorePresentationSnapshot(for: game.id) {
            resetVisiblePresentation()
        }

        let storedMainClass = game.mainClass.trimmingCharacters(
            in: .whitespacesAndNewlines
        )
        let mainClass = storedMainClass.isEmpty
            ? (try? JarMetadataReader.read(from: jarURL).mainClass)
            : storedMainClass

        guard let mainClass, !mainClass.isEmpty else {
            state = .failed(
                PhoneMECoreError.mainClassMissing.localizedDescription
            )
            return
        }

        engine.launchApplication(
            gameID: game.id,
            jarURL: jarURL,
            mainClass: mainClass,
            mediaTitle: game.title,
            mediaArtist: game.vendor,
            suiteVersion: game.version,
            mediaArtworkPath: artworkURL?.path,
            screenWidth: profile.screenWidth,
            screenHeight: profile.screenHeight,
            frameRateLimit: profile.frameRateLimit,
            framePacingMode: profile.effectiveFramePacingMode,
            heapSizeMegabytes: profile.effectiveHeapSizeMegabytes,
            autoTranslateToVietnamese: profile.isAutoTranslationEnabled,
            translationProvider: TranslationProvider.selected,
            translationSourceLanguage:
                profile.effectiveAutoTranslationSourceLanguage,
            translationTargetLanguage:
                profile.effectiveAutoTranslationTargetLanguage,
            keyUp: profile.keyCode(for: .up),
            keyDown: profile.keyCode(for: .down),
            keyLeft: profile.keyCode(for: .left),
            keyRight: profile.keyCode(for: .right),
            keyFire: profile.keyCode(for: .fire),
            keySoftLeft: profile.keyCode(for: .softLeft),
            keySoftRight: profile.keyCode(for: .softRight)
        )
    }

    func configureJIT(
        enabled: Bool,
        completion: ((Result<PhoneMECAPI.JITStatus, Error>) -> Void)? = nil
    ) {
        engine.configureJIT(enabled: enabled) { [weak self] result in
            if case let .success(status) = result {
                self?.jitStatus = status
            } else {
                self?.jitStatus = PhoneMECAPI.jitStatus
            }
            completion?(result)
        }
    }

    func refreshJITStatus() {
        engine.refreshJITStatus { [weak self] status in
            self?.jitStatus = status
        }
    }

    func setAutoTranslationConfiguration(
        enabled: Bool,
        sourceLanguage: TranslationSourceLanguage,
        targetLanguage: TranslationTargetLanguage,
        provider: TranslationProvider = .selected,
        for game: Game,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        engine.setApplicationTranslation(
            gameID: game.id,
            enabled: enabled,
            provider: provider,
            sourceLanguage: sourceLanguage,
            targetLanguage: targetLanguage,
            completion: completion
        )
    }

    func setFramePacing(
        framesPerSecond: Int,
        mode: GameProfile.FramePacingMode,
        for game: Game,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        engine.setApplicationFramePacing(
            gameID: game.id,
            framesPerSecond: framesPerSecond,
            mode: mode,
            completion: completion
        )
    }

    func hideCurrent() {
        guard let currentGame,
              let applicationState = runningApplications[currentGame.id]?.state,
              applicationState != .background else {
            return
        }

        if applicationState == .failed {
            presentationSnapshots.removeValue(forKey: currentGame.id)
            self.currentGame = nil
            state = .idle
            fpsStore.reset()
            return
        }

        // Native LCDUI does not rebuild its entire Form/List hierarchy merely
        // because NAMS assigns the same MIDlet to the foreground again. Retain
        // the last host presentation per application so reopening a hidden
        // native screen never falls back to an empty black framebuffer.
        capturePresentationSnapshot(for: currentGame.id)

        // Detach only the visible Displayable. The MIDlet isolate, network
        // sockets and native audio players stay alive so another J2ME app can
        // become foreground without destroying this one.
        engine.hideCurrentApplication()
        if var application = runningApplications[currentGame.id] {
            application.state = .background
            runningApplications[currentGame.id] = application
        }
        self.currentGame = nil
        state = .idle
        fpsStore.reset()
    }

    func terminateCurrent() {
        guard let currentGame else { return }
        terminate(gameID: currentGame.id)
    }

    func terminate(gameID: UUID) {
        fpsStore.reset()
        engine.terminateApplication(gameID: gameID)
        presentationSnapshots.removeValue(forKey: gameID)
        runningApplications.removeValue(forKey: gameID)
        if currentGame?.id == gameID {
            resetSessionResources(clearCurrentGame: true)
            state = .stopped
        }
    }

    func stop() {
        terminateCurrent()
    }

    func shutdown(
        completion: (@MainActor @Sendable () -> Void)? = nil
    ) {
        engine.stop(completion: completion)
        presentationSnapshots.removeAll(keepingCapacity: false)
        runningApplications.removeAll(keepingCapacity: false)
        resetSessionResources(clearCurrentGame: true)
        state = .stopped
    }

    func resetRuntimeForStorageChange() {
        guard runningApplications.isEmpty else { return }
        engine.resetForStorageChange()
        presentationSnapshots.removeAll(keepingCapacity: true)
        resetSessionResources(clearCurrentGame: true)
        state = .idle
    }

    func isRunning(_ gameID: UUID) -> Bool {
        runningApplications[gameID] != nil
    }

    func isRunningInBackground(_ gameID: UUID) -> Bool {
        runningApplications[gameID]?.state == .background
            || runningApplications[gameID]?.state == .paused
    }

    func suspend() {
        engine.enterBackground()
    }

    func resume() {
        engine.enterForeground()
    }

    func flushRecordStores() {
        engine.flushRecordStores()
    }

    private func capturePresentationSnapshot(for gameID: UUID) {
        presentationSnapshots[gameID] = EmulatorPresentationSnapshot(
            frame: frameStore.frame,
            presentationMode: presentationMode,
            lcdUI: lcdUI,
            lcdUIImages: lcdUIImageStore.images
        )
    }

    @discardableResult
    private func restorePresentationSnapshot(for gameID: UUID) -> Bool {
        guard let snapshot = presentationSnapshots[gameID] else {
            return false
        }

        frameStore.reset()
        if let frame = snapshot.frame {
            frameStore.update(frame)
        }
        presentationMode = snapshot.presentationMode
        lcdUI = snapshot.lcdUI
        lcdUIImageStore.replace(with: snapshot.lcdUIImages)
        return true
    }

    private func resetVisiblePresentation() {
        frameStore.reset()
        lcdUIImageStore.reset()
        lcdUI = .empty
        presentationMode = .framebuffer
    }

    private func resetSessionResources(clearCurrentGame: Bool) {
        if let gameID = currentGame?.id {
            presentationSnapshots.removeValue(forKey: gameID)
        }
        resetVisiblePresentation()
        fpsStore.reset()
        if clearCurrentGame {
            currentGame = nil
        }
    }

    func send(_ key: J2MEKey, pressed: Bool) {
        engine.sendKey(key, pressed: pressed)
    }

    func sendPointer(x: Int32, y: Int32, action: Int32) {
        engine.sendPointer(x: x, y: y, action: action)
    }

    func selectLCDUICommand(_ id: Int32) {
        engine.selectLCDUICommand(id)
    }

    func selectLCDUIListItemCommand(
        componentID: Int32,
        index: Int,
        commandID: Int32
    ) {
        updateLCDUIChoiceSelection(
            componentID: componentID,
            index: index,
            selected: true
        )
        engine.selectLCDUIListItemCommand(
            componentID: componentID,
            index: index,
            commandID: commandID
        )
    }

    func focusLCDUIItem(_ componentID: Int32) {
        if lcdUI.focusedItemID != componentID,
           lcdUI.items[componentID] != nil {
            var nextState = lcdUI
            nextState.focusedItemID = componentID
            for id in nextState.items.keys {
                nextState.items[id]?.isFocused = id == componentID
            }
            lcdUI = nextState
        }
        engine.focusLCDUIItem(componentID)
    }

    func activateLCDUIItem(_ componentID: Int32) {
        engine.activateLCDUIItem(componentID)
    }

    func setLCDUIText(
        componentID: Int32,
        text: String,
        caretPosition: Int
    ) {
        engine.setLCDUIText(
            componentID: componentID,
            text: text,
            caretPosition: caretPosition
        )
    }

    private func updateLCDUIChoiceSelection(
        componentID: Int32,
        index: Int,
        selected: Bool
    ) {
        // Reflect selection immediately instead of waiting for the VM event
        // round-trip. The native event remains authoritative and will correct
        // the state if the MIDlet changes it in response.
        if var item = lcdUI.items[componentID],
           let choiceIndex = item.choices.firstIndex(where: {
               $0.index == index
           }) {
            var changed = false
            if item.type != .multipleChoice && selected {
                for candidateIndex in item.choices.indices
                    where item.choices[candidateIndex].isSelected
                        && candidateIndex != choiceIndex {
                    item.choices[candidateIndex].isSelected = false
                    changed = true
                }
            }
            if item.choices[choiceIndex].isSelected != selected {
                item.choices[choiceIndex].isSelected = selected
                changed = true
            }
            if changed {
                var nextState = lcdUI
                nextState.items[componentID] = item
                lcdUI = nextState
            }
        }
    }

    func setLCDUIChoice(
        componentID: Int32,
        index: Int,
        selected: Bool
    ) {
        updateLCDUIChoiceSelection(
            componentID: componentID,
            index: index,
            selected: selected
        )
        engine.setLCDUIChoice(
            componentID: componentID,
            index: index,
            selected: selected
        )
    }

    func setLCDUIGauge(componentID: Int32, value: Int) {
        engine.setLCDUIGauge(componentID: componentID, value: value)
    }

    private static func isChoiceImageKey(
        _ key: Int32,
        componentID: Int32
    ) -> Bool {
        guard key < 0 else { return false }
        return ((-Int64(key)) >> 8) == Int64(componentID)
    }

    func setLCDUIDate(componentID: Int32, date: Date) {
        engine.setLCDUIDate(componentID: componentID, date: date)
    }

    func setLCDUIScrollPosition(_ position: Int) {
        engine.setLCDUIScrollPosition(position)
    }
}
