import SwiftUI
#if canImport(UIKit)
import UIKit
import Photos
import MetalKit
#elseif canImport(AppKit)
import AppKit
#endif

private struct KeyboardAdjustmentSnapshot {
    let profile: GameProfile
    let showKeypad: Bool
}

struct EmulatorView: View {
    @Environment(\.dismiss) private var dismiss
    @EnvironmentObject private var library: GameLibrary
    @EnvironmentObject private var profiles: GameProfileStore
    @EnvironmentObject private var session: EmulatorSession

    @AppStorage("enableActionBar") private var enableActionBar = true
    @AppStorage("enableStatusBar") private var enableStatusBar = true
    @AppStorage("keepScreenOn") private var keepScreenOn = false
    @AppStorage("playNativeChromeDefaultApplied") private var playNativeChromeDefaultApplied = false

    let game: Game
    private let closeAction: (() -> Void)?

    @State private var runtimeProfile: GameProfile
    @State private var persistedProfile: GameProfile
    @State private var showKeypad: Bool
    @State private var keyboardAdjustmentMode: KeyboardAdjustmentMode = .none
    @State private var keyboardAdjustmentSnapshot: KeyboardAdjustmentSnapshot?
    @State private var keyboardObscuresDisplay = false
    @State private var activeVirtualKeyCount = 0
    @State private var keyboardHideTask: Task<Void, Never>?
    @State private var appBarRevealTask: Task<Void, Never>?
    @State private var isAppBarTemporarilyVisible = false
    @State private var showKeyboardLayoutPicker = false
    @State private var showHiddenKeysEditor = false
    @State private var hiddenKeyDraft: Set<String> = []
    @State private var showScreenshotSaved = false
    @State private var nativeLCDUICaptureRect = CGRect.zero
    @State private var showExitConfirmation = false
    @State private var showError = false
    @State private var errorMessage = ""
    @State private var frameRateChangeIdentifier = UUID()
    @State private var hasStarted = false
    @State private var isClosing = false

    init(
        game: Game,
        profile: GameProfile,
        closeAction: (() -> Void)? = nil
    ) {
        let profile = profile.normalized()
        self.game = game
        self.closeAction = closeAction
        _runtimeProfile = State(initialValue: profile)
        _persistedProfile = State(initialValue: profile)
        _showKeypad = State(initialValue: profile.showVirtualKeyboard)
    }

    private var isAppBarVisible: Bool {
        runtimeProfile.showAppBar ?? enableActionBar
    }

    private var isStatusBarVisible: Bool {
        runtimeProfile.showStatusBar ?? enableStatusBar
    }

    private var shouldPresentAppBar: Bool {
        isAppBarVisible || isAppBarTemporarilyVisible || keyboardAdjustmentMode != .none
    }

    private var navigationTitle: String {
        guard session.isPresentingNativeLCDUI,
              session.lcdUI.screenKind != .alert,
              let title = session.lcdUI.screen?.title.trimmingCharacters(
                  in: .whitespacesAndNewlines
              ),
              !title.isEmpty else {
            return game.title
        }
        return title
    }

    private var presentsFullscreenSurface: Bool {
        guard !session.isPresentingNativeLCDUI else { return false }
        return runtimeProfile.forceFullscreen || session.lcdUI.isCanvasFullScreen
    }

    private var showsCanvasCommandBar: Bool {
        !presentsFullscreenSurface && !session.lcdUI.commands.isEmpty
    }

    // MIDP fullscreen controls the emulated Canvas chrome only. It must not
    // override the user's native ActionBar/status-bar preferences.
    private var presentsEdgeToEdgeSurface: Bool {
        presentsFullscreenSurface && !isAppBarVisible && !isStatusBarVisible
    }

    var body: some View {
        PhoneMENavigationStack {
            GeometryReader { geometry in
                ZStack {
                    Color.playSurfaceBackground
                        .ignoresSafeArea()

                    // Keep the system menu outside the frequently invalidated
                    // Canvas subtree. Otherwise FPS/LCDUI updates can dismiss
                    // an open toolbar menu or its nested submenu on iOS.
                    EmulatorToolbarAnchor(
                        keyboardDisabled: session.isPresentingNativeLCDUI,
                        keyboardAdjustmentMode: keyboardAdjustmentMode,
                        isRotationLocked: runtimeProfile.lockedOrientation != nil,
                        translationEnabled: runtimeProfile.isAutoTranslationEnabled,
                        translationSourceLanguage:
                            runtimeProfile.effectiveAutoTranslationSourceLanguage,
                        translationTargetLanguage:
                            runtimeProfile.effectiveAutoTranslationTargetLanguage,
                        frameRateOverrideEnabled:
                            runtimeProfile.isFrameRateOverrideEnabled,
                        frameRateLimit: GameProfile.resolvedFrameRate(
                            runtimeProfile.frameRateLimit
                        ),
                        isAppBarVisible: isAppBarVisible,
                        toggleAppBarAction: toggleAppBarVisibility,
                        hideAction: hideApplication,
                        exitAction: { showExitConfirmation = true },
                        toggleRotationLockAction: toggleRotationLock,
                        setTranslationConfigurationAction:
                            setAutoTranslationConfiguration,
                        setFrameRateAction: setQuickFrameRate,
                        toggleKeyboardAction: toggleKeyboard,
                        screenshotAction: saveScreenshot,
                        beginKeyboardPositionAction: {
                            beginKeyboardAdjustment(.position)
                        },
                        beginKeyboardResizeAction: {
                            beginKeyboardAdjustment(.size)
                        },
                        finishKeyboardAdjustmentAction: finishKeyboardAdjustment,
                        discardKeyboardAdjustmentAction: discardKeyboardAdjustment,
                        switchKeyboardLayoutAction: {
                            showKeyboardLayoutPicker = true
                        },
                        hideKeyboardButtonsAction: openHiddenKeysEditor,
                        resetKeyboardLayoutAction: resetKeyboardLayout
                    )
                    .equatable()

                    if session.isPresentingNativeLCDUI {
                        NativeLCDUIScreenView(
                            imageStore: session.lcdUIImageStore,
                            state: session.lcdUI,
                            showsTitleInContent: !isAppBarVisible
                                || runtimeProfile.forceFullscreen
                        )
                            .environmentObject(session)
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                            .background {
                                GeometryReader { captureGeometry in
                                    Color.clear.preference(
                                        key: NativeLCDUICaptureRectPreferenceKey.self,
                                        value: captureGeometry.frame(
                                            in: .named("emulatorCaptureWindow")
                                        )
                                    )
                                }
                            }
                    } else {
                        VStack(spacing: 0) {
                            GeometryReader { canvasGeometry in
                                let displayRect = FrameLayout.renderedFrameRect(
                                    frame: nil,
                                    availableSize: canvasGeometry.size,
                                    profile: runtimeProfile
                                )

                                ZStack {
                                    FrameSurface(
                                        frameStore: session.frameStore,
                                        profile: runtimeProfile
                                    )
                                        .environmentObject(session)
                                        .frame(maxWidth: .infinity, maxHeight: .infinity)

                                    if showKeypad {
                                        KeypadView(
                                            profile: $runtimeProfile,
                                            editMode: keyboardAdjustmentMode,
                                            layoutRect: keyboardFrame(
                                                in: canvasGeometry.size
                                            ),
                                            displayRect: displayRect,
                                            onKeyActivity: { active in
                                                handleVirtualKeyActivity(
                                                    active,
                                                    obscuresDisplay: keyboardObscuresDisplay
                                                )
                                            },
                                            onObscuresDisplayChange: updateKeyboardOverlap
                                        )
                                        .environmentObject(session)
                                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                                        .transition(.opacity)
                                    }
                                }
                            }

                            if showsCanvasCommandBar {
                                LCDUICommandBar(state: session.lcdUI)
                                    .environmentObject(session)
                            }
                        }
                    }

                    if runtimeProfile.showFPS,
                       session.state == .running,
                       !session.isPresentingNativeLCDUI {
                        EmulatorFPSOverlay(fpsStore: session.fpsStore)
                    }

                    if session.state == .starting {
                        EmulatorLoadingOverlay(
                            title: L10n.format("Starting %@", game.title)
                        )
                    }
                }
                .coordinateSpace(name: "emulatorSurface")
                .contentShape(Rectangle())
            }
            .onPreferenceChange(NativeLCDUICaptureRectPreferenceKey.self) { rect in
                nativeLCDUICaptureRect = rect
            }
            .ignoresSafeArea(
                .container,
                edges: presentsEdgeToEdgeSurface
                    ? .all
                    : (isAppBarTemporarilyVisible && !isAppBarVisible ? .top : [])
            )
            .simultaneousGesture(appBarRevealGesture)
#if os(iOS)
            .navigationTitle(navigationTitle)
            .navigationBarTitleDisplayMode(.inline)
            .phoneMENavigationBarBackgroundVisible()
            .phoneMENavigationBarVisible(shouldPresentAppBar)
#else
            .navigationTitle(navigationTitle)
#endif
        }
        .coordinateSpace(name: "emulatorCaptureWindow")
#if os(iOS)
        .statusBarHidden(!isStatusBarVisible)
#endif
        .onAppear {
            if !playNativeChromeDefaultApplied {
                enableActionBar = true
                enableStatusBar = true
                playNativeChromeDefaultApplied = true
            }

            configureScreenAwake(true)
            applyEffectiveOrientation()
            scheduleKeyboardAutoHide(obscuresDisplay: keyboardObscuresDisplay)
            guard !hasStarted else { return }
            hasStarted = true
            launch()
        }
        .onDisappear {
            keyboardHideTask?.cancel()
            appBarRevealTask?.cancel()
            if let snapshot = keyboardAdjustmentSnapshot {
                runtimeProfile = snapshot.profile
                showKeypad = snapshot.showKeypad
                keyboardAdjustmentSnapshot = nil
                keyboardAdjustmentMode = .none
            }
            persistRuntimeProfile()
            configureScreenAwake(false)
            resetPreferredOrientation()
            session.hideCurrent()
        }
        .onChange(of: showKeypad) { visible in
            if visible {
                scheduleKeyboardAutoHide(obscuresDisplay: keyboardObscuresDisplay)
            } else {
                keyboardHideTask?.cancel()
                activeVirtualKeyCount = 0
            }
        }
        .onChange(of: session.state) { state in
            switch state {
            case let .failed(message):
                errorMessage = message
                showError = true

            case .stopped where hasStarted && !isClosing:
                close(stopSession: false)

            default:
                break
            }
        }
        .confirmationDialog(
            "Switch keylayout",
            isPresented: $showKeyboardLayoutPicker,
            titleVisibility: .visible
        ) {
            ForEach(
                KeyboardLayoutCatalog.selectableLayouts(
                    hasCustomLayout: runtimeProfile.keyboardLayoutCustomization != nil
                        || runtimeProfile.keyboardCustomBaseType != nil
                )
            ) { layout in
                Button(layoutPickerTitle(layout)) {
                    selectKeyboardLayout(layout)
                }
            }
        }
        .sheet(isPresented: $showHiddenKeysEditor) {
            PhoneMENavigationStack {
                KeyboardVisibilityEditor(
                    controls: KeyboardLayoutCatalog.controlChoices(for: runtimeProfile),
                    hiddenControlIDs: $hiddenKeyDraft,
                    applyAction: applyHiddenKeyChanges
                )
            }
        }
        .alert("Screenshot saved", isPresented: $showScreenshotSaved) {
            Button("OK", role: .cancel) {}
        } message: {
            Text("The screenshot was added to Photos.")
        }
        .alert("Error", isPresented: $showError) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(errorMessage)
        }
        .alert("Exit application?", isPresented: $showExitConfirmation) {
            Button("Cancel", role: .cancel) {}
            Button("Exit", role: .destructive) {
                close()
            }
        } message: {
            Text("Unsaved progress may be lost.")
        }
    }

    private func controlsMaxWidth(for size: CGSize) -> CGFloat {
        max(size.width - 12, 0)
    }

    private func controlsHeight(for size: CGSize) -> CGFloat {
        size.width > size.height ? min(size.height * 0.50, 280) : min(size.height * 0.42, 320)
    }

    private func keyboardFrame(in size: CGSize) -> CGRect {
        let width = controlsMaxWidth(for: size)
        let height = controlsHeight(for: size)
        return CGRect(
            x: (size.width - width) / 2,
            y: size.height - height - 8,
            width: width,
            height: height
        )
    }

    private func updateKeyboardOverlap(_ obscuresDisplay: Bool) {
        keyboardObscuresDisplay = obscuresDisplay
        if showKeypad, activeVirtualKeyCount == 0 {
            scheduleKeyboardAutoHide(obscuresDisplay: obscuresDisplay)
        }
    }

    private func setAutoTranslationConfiguration(
        _ enabled: Bool,
        _ sourceLanguage: TranslationSourceLanguage,
        _ targetLanguage: TranslationTargetLanguage
    ) {
        let previousEnabled = runtimeProfile.isAutoTranslationEnabled
        let previousSourceLanguage =
            runtimeProfile.effectiveAutoTranslationSourceLanguage
        let previousTargetLanguage =
            runtimeProfile.effectiveAutoTranslationTargetLanguage
        guard previousEnabled != enabled ||
                previousSourceLanguage != sourceLanguage ||
                previousTargetLanguage != targetLanguage else {
            return
        }

        runtimeProfile.setAutoTranslationEnabled(enabled)
        if enabled {
            runtimeProfile.effectiveAutoTranslationSourceLanguage =
                sourceLanguage
            runtimeProfile.effectiveAutoTranslationTargetLanguage =
                targetLanguage
        }
        persistRuntimeProfile()
        session.setAutoTranslationConfiguration(
            enabled: enabled,
            sourceLanguage: sourceLanguage,
            targetLanguage: targetLanguage,
            for: game
        ) { result in
            guard case let .failure(error) = result else { return }
            runtimeProfile.setAutoTranslationEnabled(previousEnabled)
            runtimeProfile.effectiveAutoTranslationSourceLanguage =
                previousSourceLanguage
            runtimeProfile.effectiveAutoTranslationTargetLanguage =
                previousTargetLanguage
            persistRuntimeProfile()
            errorMessage = error.localizedDescription
            showError = true
        }
    }

    private func setQuickFrameRate(_ framesPerSecond: Int?) {
        let previousProfile = runtimeProfile
        var updatedProfile = runtimeProfile
        if let framesPerSecond {
            updatedProfile.frameRateLimit = framesPerSecond
            updatedProfile.isFrameRateOverrideEnabled = true
        } else {
            updatedProfile.frameRateLimit = GameProfile.defaultFrameRate
            updatedProfile.isFrameRateOverrideEnabled = false
        }
        updatedProfile.normalize()

        guard updatedProfile.frameRateLimit != previousProfile.frameRateLimit ||
                updatedProfile.effectiveFramePacingMode !=
                    previousProfile.effectiveFramePacingMode else {
            return
        }

        let changeIdentifier = UUID()
        frameRateChangeIdentifier = changeIdentifier
        runtimeProfile = updatedProfile
        persistRuntimeProfile()
        session.setFramePacing(
            framesPerSecond: updatedProfile.frameRateLimit,
            mode: updatedProfile.effectiveFramePacingMode,
            for: game
        ) { result in
            guard frameRateChangeIdentifier == changeIdentifier,
                  case let .failure(error) = result else {
                return
            }
            runtimeProfile = previousProfile
            persistRuntimeProfile()
            errorMessage = error.localizedDescription
            showError = true
        }
    }

    private var appBarRevealGesture: some Gesture {
        DragGesture(minimumDistance: 12, coordinateSpace: .local)
            .onEnded { value in
                guard !isAppBarVisible,
                      keyboardAdjustmentMode == .none,
                      value.startLocation.y <= 28,
                      value.translation.height >= 32,
                      abs(value.translation.height) > abs(value.translation.width) else {
                    return
                }
                revealAppBarTemporarily()
            }
    }

    private func toggleAppBarVisibility() {
        appBarRevealTask?.cancel()
        runtimeProfile.showAppBar = !isAppBarVisible
        isAppBarTemporarilyVisible = false
        persistRuntimeProfile()
    }

    private func revealAppBarTemporarily() {
        guard !isAppBarVisible, keyboardAdjustmentMode == .none else { return }

        appBarRevealTask?.cancel()
        withAnimation(.easeInOut(duration: 0.18)) {
            isAppBarTemporarilyVisible = true
        }

        appBarRevealTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 8_000_000_000)
            guard !Task.isCancelled,
                  !isAppBarVisible,
                  keyboardAdjustmentMode == .none else {
                return
            }
            withAnimation(.easeInOut(duration: 0.18)) {
                isAppBarTemporarilyVisible = false
            }
        }
    }

    private func toggleKeyboard() {
        keyboardHideTask?.cancel()
        if keyboardAdjustmentMode != .none {
            finishKeyboardAdjustment()
            return
        }

        let visible = !showKeypad
        withAnimation(.easeInOut(duration: 0.15)) {
            showKeypad = visible
        }
        runtimeProfile.showVirtualKeyboard = visible
        persistRuntimeProfile()
    }

    private func handleVirtualKeyActivity(
        _ active: Bool,
        obscuresDisplay: Bool
    ) {
        if active {
            activeVirtualKeyCount += 1
            keyboardHideTask?.cancel()
        } else {
            activeVirtualKeyCount = max(0, activeVirtualKeyCount - 1)
            if activeVirtualKeyCount == 0 {
                scheduleKeyboardAutoHide(obscuresDisplay: obscuresDisplay)
            }
        }
    }

    private func scheduleKeyboardAutoHide(obscuresDisplay: Bool) {
        keyboardHideTask?.cancel()
        guard showKeypad,
              keyboardAdjustmentMode == .none,
              activeVirtualKeyCount == 0,
              obscuresDisplay,
              runtimeProfile.keyboardHideDelayMilliseconds > 0 else {
            return
        }

        let milliseconds = min(runtimeProfile.keyboardHideDelayMilliseconds, 60_000)
        keyboardHideTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: UInt64(milliseconds) * 1_000_000)
            guard !Task.isCancelled,
                  activeVirtualKeyCount == 0,
                  keyboardAdjustmentMode == .none else {
                return
            }
            withAnimation(.easeInOut(duration: 0.15)) {
                showKeypad = false
            }
        }
    }

    private func beginKeyboardAdjustment(_ mode: KeyboardAdjustmentMode) {
        keyboardHideTask?.cancel()
        if keyboardAdjustmentMode == .none {
            keyboardAdjustmentSnapshot = KeyboardAdjustmentSnapshot(
                profile: runtimeProfile,
                showKeypad: showKeypad
            )
        }
        prepareKeyboardForEditing()
        keyboardAdjustmentMode = mode
        runtimeProfile.showVirtualKeyboard = true
        withAnimation(.easeInOut(duration: 0.15)) {
            showKeypad = true
        }
    }

    private func finishKeyboardAdjustment() {
        guard keyboardAdjustmentMode != .none else { return }
        keyboardAdjustmentMode = .none
        keyboardAdjustmentSnapshot = nil
        runtimeProfile.makeKeyboardLayoutCustom()
        persistRuntimeProfile()
        scheduleKeyboardAutoHide(obscuresDisplay: keyboardObscuresDisplay)
    }

    private func discardKeyboardAdjustment() {
        guard let snapshot = keyboardAdjustmentSnapshot else { return }
        keyboardHideTask?.cancel()
        keyboardAdjustmentMode = .none
        runtimeProfile = snapshot.profile
        keyboardAdjustmentSnapshot = nil
        activeVirtualKeyCount = 0
        withAnimation(.easeInOut(duration: 0.15)) {
            showKeypad = snapshot.showKeypad
        }
        scheduleKeyboardAutoHide(obscuresDisplay: keyboardObscuresDisplay)
    }

    private func selectKeyboardLayout(_ layout: GameProfile.VirtualKeyboardType) {
        keyboardHideTask?.cancel()
        keyboardAdjustmentMode = .none
        runtimeProfile.virtualKeyboardType = layout
        runtimeProfile.showVirtualKeyboard = true
        if layout != .custom {
            runtimeProfile.keyboardTuning = nil
        }
        withAnimation(.easeInOut(duration: 0.15)) {
            showKeypad = true
        }
        persistRuntimeProfile()
        scheduleKeyboardAutoHide(obscuresDisplay: keyboardObscuresDisplay)
    }

    private func openHiddenKeysEditor() {
        keyboardHideTask?.cancel()
        keyboardAdjustmentMode = .none
        hiddenKeyDraft = runtimeProfile.effectiveKeyboardLayoutCustomization.hiddenControlIDs
        showHiddenKeysEditor = true
    }

    private func applyHiddenKeyChanges() {
        prepareKeyboardForEditing()
        runtimeProfile.updateKeyboardLayoutCustomization { customization in
            customization.hiddenControlIDs = hiddenKeyDraft
        }
        runtimeProfile.showVirtualKeyboard = true
        showKeypad = true
        showHiddenKeysEditor = false
        persistRuntimeProfile()
    }

    private func prepareKeyboardForEditing() {
        guard runtimeProfile.virtualKeyboardType != .custom else { return }
        let baseType = runtimeProfile.resolvedKeyboardBaseType
        runtimeProfile.keyboardLayoutCustomization = nil
        runtimeProfile.keyboardCustomBaseType = baseType
        runtimeProfile.virtualKeyboardType = .custom
    }

    private func resetKeyboardLayout() {
        keyboardHideTask?.cancel()
        keyboardAdjustmentMode = .none
        runtimeProfile.resetKeyboardLayoutCustomization()
        runtimeProfile.virtualKeyboardType = .arrowsNumbers
        runtimeProfile.showVirtualKeyboard = true
        showKeypad = true
        persistRuntimeProfile()
    }

    private func persistRuntimeProfile() {
        runtimeProfile.normalize()
        persistedProfile = runtimeProfile
        profiles.save(persistedProfile, for: game)
    }

    private func layoutPickerTitle(_ layout: GameProfile.VirtualKeyboardType) -> String {
        let selected = runtimeProfile.virtualKeyboardType == layout
        return selected ? "✓ \(layout.title)" : layout.title
    }

    private func launch() {
        do {
            let jarURL = try library.prepareJarForLaunch(game)
            library.markPlayed(game)
            session.launch(
                game: game,
                jarURL: jarURL,
                artworkURL: library.iconURL(for: game),
                profile: runtimeProfile
            )
        } catch {
            errorMessage = error.localizedDescription
            showError = true
        }
    }

    private func hideApplication() {
        guard !isClosing else { return }
        isClosing = true
        keyboardHideTask?.cancel()
        persistRuntimeProfile()
        session.hideCurrent()
        if let closeAction {
            closeAction()
        } else {
            dismiss()
        }
    }

    private func close(stopSession: Bool = true) {
        guard !isClosing else { return }
        isClosing = true
        keyboardHideTask?.cancel()
        persistRuntimeProfile()
        if stopSession {
            session.stop()
        }
        if let closeAction {
            closeAction()
        } else {
            dismiss()
        }
    }

    private func saveScreenshot() {
#if os(iOS)
        guard let image = captureCurrentScreen() else {
            errorMessage = L10n.string("Unable to capture the current screen.")
            showError = true
            return
        }

        let saveImage = {
            PHPhotoLibrary.shared().performChanges({
                PHAssetChangeRequest.creationRequestForAsset(from: image)
            }) { success, error in
                DispatchQueue.main.async {
                    if success {
                        showScreenshotSaved = true
                    } else {
                        errorMessage = error?.localizedDescription
                            ?? L10n.string(
                                "Unable to save the screenshot to Photos."
                            )
                        showError = true
                    }
                }
            }
        }

        switch PHPhotoLibrary.authorizationStatus(for: .addOnly) {
        case .authorized, .limited:
            saveImage()
        case .notDetermined:
            PHPhotoLibrary.requestAuthorization(for: .addOnly) { status in
                DispatchQueue.main.async {
                    if status == .authorized || status == .limited {
                        saveImage()
                    } else {
                        errorMessage = L10n.string(
                            "Photos access is required to save screenshots."
                        )
                        showError = true
                    }
                }
            }
        default:
            errorMessage = L10n.string(
                "Photos access is required to save screenshots."
            )
            showError = true
        }
#endif
    }

#if os(iOS)
    private func captureCurrentScreen() -> UIImage? {
        if !session.isPresentingNativeLCDUI {
            guard
                let frame = session.frame,
                let image = frame.makeCGImage()
            else {
                return nil
            }
            return UIImage(cgImage: image, scale: 1, orientation: .up)
        }

        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }),
              let window = scene.windows.first(where: \.isKeyWindow),
              let captureRect = alignedCaptureRect(
                nativeLCDUICaptureRect,
                inside: window.bounds,
                scale: scene.screen.scale
              ) else {
            return nil
        }

        let format = UIGraphicsImageRendererFormat.default()
        format.scale = scene.screen.scale
        format.opaque = true
        let renderer = UIGraphicsImageRenderer(size: captureRect.size, format: format)
        return renderer.image { _ in
            window.drawHierarchy(
                in: window.bounds.offsetBy(
                    dx: -captureRect.minX,
                    dy: -captureRect.minY
                ),
                afterScreenUpdates: true
            )
        }
    }

    private func alignedCaptureRect(
        _ rect: CGRect,
        inside bounds: CGRect,
        scale: CGFloat
    ) -> CGRect? {
        let clippedRect = rect.standardized.intersection(bounds)
        guard !clippedRect.isNull,
              clippedRect.width > 0,
              clippedRect.height > 0,
              scale > 0 else {
            return nil
        }

        let minX = floor(clippedRect.minX * scale) / scale
        let minY = floor(clippedRect.minY * scale) / scale
        let maxX = ceil(clippedRect.maxX * scale) / scale
        let maxY = ceil(clippedRect.maxY * scale) / scale
        let alignedRect = CGRect(
            x: minX,
            y: minY,
            width: maxX - minX,
            height: maxY - minY
        ).intersection(bounds)

        guard !alignedRect.isNull,
              alignedRect.width > 0,
              alignedRect.height > 0 else {
            return nil
        }
        return alignedRect
    }
#endif

    private func toggleRotationLock() {
#if os(iOS)
        if runtimeProfile.lockedOrientation == nil {
            runtimeProfile.lockedOrientation = currentInterfaceOrientation()
        } else {
            runtimeProfile.lockedOrientation = nil
        }
        persistRuntimeProfile()
        applyEffectiveOrientation()
#endif
    }

    private func currentInterfaceOrientation() -> GameProfile.Orientation {
#if os(iOS)
        guard let orientation = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive })?
            .interfaceOrientation else {
            return .portrait
        }
        return orientation.isLandscape ? .landscape : .portrait
#else
        return .portrait
#endif
    }

    private func configureScreenAwake(_ active: Bool) {
#if canImport(UIKit)
        UIApplication.shared.isIdleTimerDisabled = active && keepScreenOn
#endif
    }

    private func applyEffectiveOrientation() {
        applyPreferredOrientation(
            runtimeProfile.lockedOrientation ?? runtimeProfile.orientation
        )
    }

    private func applyPreferredOrientation(_ orientation: GameProfile.Orientation) {
#if os(iOS)
        let mask: UIInterfaceOrientationMask
        switch orientation {
        case .defaultValue, .auto:
            mask = .allButUpsideDown
        case .portrait:
            mask = .portrait
        case .landscape:
            mask = .landscape
        }
        requestOrientation(mask)
#endif
    }

    private func resetPreferredOrientation() {
#if os(iOS)
        requestOrientation(.allButUpsideDown)
#endif
    }

#if os(iOS)
    private func requestOrientation(_ mask: UIInterfaceOrientationMask) {
        PhoneMEAppDelegate.supportedOrientationMask = mask

        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene })
            .first(where: { $0.activationState == .foregroundActive }) else {
            return
        }

        if #available(iOS 16.0, *) {
            scene.windows
                .first(where: \.isKeyWindow)?
                .rootViewController?
                .setNeedsUpdateOfSupportedInterfaceOrientations()
            scene.requestGeometryUpdate(
                .iOS(interfaceOrientations: mask)
            ) { _ in }
        } else {
            UIViewController.attemptRotationToDeviceOrientation()
        }
    }
#endif
}

private struct NativeLCDUICaptureRectPreferenceKey: PreferenceKey {
    static var defaultValue: CGRect { .zero }

    static func reduce(value: inout CGRect, nextValue: () -> CGRect) {
        value = nextValue()
    }
}

private struct EmulatorLoadingOverlay: View {
    let title: String

    var body: some View {
        ZStack {
            Rectangle()
                .fill(.regularMaterial)
                .ignoresSafeArea()

            ProgressView(title)
                .controlSize(.large)
        }
        .accessibilityElement(children: .combine)
    }
}

private struct EmulatorFPSOverlay: View {
    @ObservedObject var fpsStore: EmulatorFPSStore

    var body: some View {
        Text(String(format: "%.1f FPS", fpsStore.value))
            .font(.caption.monospacedDigit().weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(.regularMaterial, in: Capsule())
            .frame(
                maxWidth: .infinity,
                maxHeight: .infinity,
                alignment: .topTrailing
            )
            .padding(10)
            .allowsHitTesting(false)
    }
}

private struct EmulatorToolbarAnchor: View, Equatable {
    let keyboardDisabled: Bool
    let keyboardAdjustmentMode: KeyboardAdjustmentMode
    let isRotationLocked: Bool
    let translationEnabled: Bool
    let translationSourceLanguage: TranslationSourceLanguage
    let translationTargetLanguage: TranslationTargetLanguage
    let frameRateOverrideEnabled: Bool
    let frameRateLimit: Int
    let isAppBarVisible: Bool
    let toggleAppBarAction: () -> Void
    let hideAction: () -> Void
    let exitAction: () -> Void
    let toggleRotationLockAction: () -> Void
    let setTranslationConfigurationAction:
        (Bool, TranslationSourceLanguage, TranslationTargetLanguage) -> Void
    let setFrameRateAction: (Int?) -> Void
    let toggleKeyboardAction: () -> Void
    let screenshotAction: () -> Void
    let beginKeyboardPositionAction: () -> Void
    let beginKeyboardResizeAction: () -> Void
    let finishKeyboardAdjustmentAction: () -> Void
    let discardKeyboardAdjustmentAction: () -> Void
    let switchKeyboardLayoutAction: () -> Void
    let hideKeyboardButtonsAction: () -> Void
    let resetKeyboardLayoutAction: () -> Void

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.keyboardDisabled == rhs.keyboardDisabled
            && lhs.keyboardAdjustmentMode == rhs.keyboardAdjustmentMode
            && lhs.isRotationLocked == rhs.isRotationLocked
            && lhs.translationEnabled == rhs.translationEnabled
            && lhs.translationSourceLanguage == rhs.translationSourceLanguage
            && lhs.translationTargetLanguage == rhs.translationTargetLanguage
            && lhs.frameRateOverrideEnabled == rhs.frameRateOverrideEnabled
            && lhs.frameRateLimit == rhs.frameRateLimit
            && lhs.isAppBarVisible == rhs.isAppBarVisible
    }

    var body: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .toolbar {
                ToolbarItem(placement: .principal) {
                    Group {
                        if keyboardAdjustmentMode != .none {
                            Menu {
                            Button(action: beginKeyboardPositionAction) {
                                Label(
                                    "Move keys",
                                    systemImage: keyboardAdjustmentMode == .position
                                        ? "checkmark.circle.fill"
                                        : "arrow.up.and.down.and.arrow.left.and.right"
                                )
                            }
                            Button(action: beginKeyboardResizeAction) {
                                Label(
                                    "Resize key groups",
                                    systemImage: keyboardAdjustmentMode == .size
                                        ? "checkmark.circle.fill"
                                        : "arrow.up.left.and.arrow.down.right"
                                )
                            }
                        } label: {
                            Label(
                                keyboardAdjustmentMode == .position
                                    ? L10n.string("Move Virtual Keys")
                                    : L10n.string("Resize Key Groups"),
                                systemImage: keyboardAdjustmentMode == .position
                                    ? "arrow.up.and.down.and.arrow.left.and.right"
                                    : "arrow.up.left.and.arrow.down.right"
                            )
                                .font(.subheadline.weight(.semibold))
                            }
                            .accessibilityLabel("Keyboard edit mode")
                        }
                    }
                }

                ToolbarItem(placement: .cancellationAction) {
                    Group {
                        if keyboardAdjustmentMode != .none {
                            Button(
                                "Discard",
                                role: .cancel,
                                action: discardKeyboardAdjustmentAction
                            )
                        }
                    }
                }

                ToolbarItem(placement: .primaryAction) {
                    Group {
                        if keyboardAdjustmentMode != .none {
                            Button("Done", action: finishKeyboardAdjustmentAction)
                                .font(.body.weight(.semibold))
                        }
                    }
                }

                ToolbarItem(placement: .primaryAction) {
                    Group {
                        if keyboardAdjustmentMode == .none {
                            Button(action: toggleKeyboardAction) {
                                Image(systemName: "keyboard")
                            }
                            .accessibilityLabel("Keyboard (IME)")
                            .disabled(keyboardDisabled)
                        }
                    }
                }

                ToolbarItem(placement: .primaryAction) {
                    Group {
                        if keyboardAdjustmentMode == .none {
                            Button(action: screenshotAction) {
                                Image(systemName: "camera")
                            }
                            .accessibilityLabel("Take screenshot")
                        }
                    }
                }

                ToolbarItem(placement: .primaryAction) {
                    Group {
                        if keyboardAdjustmentMode == .none {
                    Menu {
                        Menu {
                            Button(action: beginKeyboardPositionAction) {
                                Label(
                                    "Move keys",
                                    systemImage: "arrow.up.and.down.and.arrow.left.and.right"
                                )
                            }
                            Button(action: beginKeyboardResizeAction) {
                                Label(
                                    "Resize key groups",
                                    systemImage: "arrow.up.left.and.arrow.down.right"
                                )
                            }
                            if keyboardAdjustmentMode != .none {
                                Button(action: finishKeyboardAdjustmentAction) {
                                    Label("Finish editing", systemImage: "checkmark")
                                }
                            }
                            Divider()
                            Button(action: switchKeyboardLayoutAction) {
                                Label("Choose layout", systemImage: "keyboard")
                            }
                            .disabled(keyboardAdjustmentMode != .none)
                            Button(action: hideKeyboardButtonsAction) {
                                Label("Visible buttons", systemImage: "eye")
                            }
                            .disabled(keyboardAdjustmentMode != .none)
                            Button(
                                role: .destructive,
                                action: resetKeyboardLayoutAction
                            ) {
                                Label(
                                    "Reset keyboard layout",
                                    systemImage: "arrow.counterclockwise"
                                )
                            }
                        } label: {
                            Label("Virtual keyboard", systemImage: "keyboard")
                        }
                        Divider()
                        Button(action: toggleRotationLockAction) {
                            Label(
                                isRotationLocked
                                    ? L10n.string("Unlock screen rotation")
                                    : L10n.string("Lock screen rotation"),
                                systemImage: isRotationLocked
                                    ? "lock.open"
                                    : "lock"
                            )
                        }
                        Menu {
                            Button {
                                setTranslationConfigurationAction(
                                    false,
                                    translationSourceLanguage,
                                    translationTargetLanguage
                                )
                            } label: {
                                Label(
                                    "Off",
                                    systemImage: !translationEnabled
                                        ? "checkmark.circle.fill"
                                        : "nosign"
                                )
                            }
                            Divider()
                            Menu {
                                ForEach(TranslationSourceLanguage.allCases) { language in
                                    Button {
                                        setTranslationConfigurationAction(
                                            true,
                                            language,
                                            translationTargetLanguage
                                        )
                                    } label: {
                                        Label(
                                            language.title,
                                            systemImage: translationEnabled &&
                                                translationSourceLanguage == language
                                                ? "checkmark.circle.fill"
                                                : language.systemImage
                                        )
                                    }
                                }
                            } label: {
                                Label(
                                    L10n.format(
                                        "Translate from: %@",
                                        translationSourceLanguage.title
                                    ),
                                    systemImage: "arrow.right"
                                )
                            }
                            Menu {
                                ForEach(TranslationTargetLanguage.allCases) { language in
                                    Button {
                                        setTranslationConfigurationAction(
                                            true,
                                            translationSourceLanguage,
                                            language
                                        )
                                    } label: {
                                        Label(
                                            language.title,
                                            systemImage: translationEnabled &&
                                                translationTargetLanguage == language
                                                ? "checkmark.circle.fill"
                                                : language.systemImage
                                        )
                                    }
                                }
                            } label: {
                                Label(
                                    L10n.format(
                                        "Translate to: %@",
                                        translationTargetLanguage.title
                                    ),
                                    systemImage: "arrow.left"
                                )
                            }
                        } label: {
                            Label(
                                "Auto translate",
                                systemImage: "character.book.closed.fill"
                            )
                        }
                        Menu {
                            Button {
                                setFrameRateAction(nil)
                            } label: {
                                Label(
                                    "Default",
                                    systemImage: !frameRateOverrideEnabled
                                        ? "checkmark.circle.fill"
                                        : "clock"
                                )
                            }
                            Divider()
                            ForEach([30, 60], id: \.self) { fps in
                                Button {
                                    setFrameRateAction(fps)
                                } label: {
                                    Label(
                                        "\(fps) FPS",
                                        systemImage: frameRateOverrideEnabled &&
                                            frameRateLimit == fps
                                            ? "checkmark.circle.fill"
                                            : "speedometer"
                                    )
                                }
                            }
                        } label: {
                            Label("FPS", systemImage: "speedometer")
                        }
                        Divider()
                        Button(action: toggleAppBarAction) {
                            Label(
                                isAppBarVisible ? "Hide App Bar" : "Show App Bar",
                                systemImage: isAppBarVisible
                                    ? "chevron.up"
                                    : "chevron.down"
                            )
                        }
                        Button(action: hideAction) {
                            Label("Hide application", systemImage: "rectangle.portrait.and.arrow.right")
                        }
                        Button(role: .destructive, action: exitAction) {
                            Label("Exit", systemImage: "power")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                            .accessibilityLabel("More")
                        }
                    }
                }
            }
    }
}

private struct KeyboardVisibilityEditor: View {
    @Environment(\.dismiss) private var dismiss

    let controls: [KeyboardControlDescriptor]
    @Binding var hiddenControlIDs: Set<String>
    let applyAction: () -> Void

    var body: some View {
        List {
            Section {
                ForEach(controls) { control in
                    Toggle(
                        control.accessibilityLabel,
                        isOn: visibilityBinding(for: control.id)
                    )
                }
            } header: {
                Text("Visible Controls")
            }
        }
        .listStyle(.insetGrouped)
        .navigationTitle("Virtual Buttons")
#if os(iOS)
        .navigationBarTitleDisplayMode(.inline)
#endif
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") {
                    dismiss()
                }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Done", action: applyAction)
            }
        }
    }

    private func visibilityBinding(for id: String) -> Binding<Bool> {
        Binding(
            get: { !hiddenControlIDs.contains(id) },
            set: { visible in
                if visible {
                    hiddenControlIDs.remove(id)
                } else {
                    hiddenControlIDs.insert(id)
                }
            }
        )
    }
}

struct FrameSurface: View {
    @EnvironmentObject private var session: EmulatorSession
    @ObservedObject var frameStore: EmulatorFrameStore

    let profile: GameProfile
    var capturesHardwareKeyboard = true
    var fitsEntireFrame = false
    var forcesNearestNeighborFit = false

    @State private var pointerIsDown = false

    var body: some View {
        GeometryReader { geometry in
            if let frameSize = frameStore.frameSize {
                let rect = FrameLayout.renderedFrameRect(
                    frameSize: frameSize,
                    availableSize: geometry.size,
                    profile: profile,
                    strictFit: fitsEntireFrame,
                    forceFit: forcesNearestNeighborFit
                )

                ZStack(alignment: .topLeading) {
                    renderedFrame()
                        .frame(
                            width: max(rect.width, 0),
                            height: max(rect.height, 0)
                        )
                        .offset(x: rect.minX, y: rect.minY)

#if canImport(UIKit)
                    if capturesHardwareKeyboard {
                        PhoneMEHardwareKeyboardView { key, pressed in
                            session.send(key, pressed: pressed)
                        }
                        .frame(
                            width: geometry.size.width,
                            height: geometry.size.height
                        )
                    }
#endif
                }
                .frame(
                    width: geometry.size.width,
                    height: geometry.size.height,
                    alignment: .topLeading
                )
                .clipped()
                .contentShape(Rectangle())
                .gesture(pointerGesture(
                    frameSize: frameSize,
                    availableSize: geometry.size
                ))
            }
        }
        .accessibilityLabel("J2ME display")
    }

    @ViewBuilder
    private func renderedFrame() -> some View {
#if canImport(UIKit)
        PhoneMEFrameLayerView(
            frameStore: frameStore,
            filtering: forcesNearestNeighborFit ? false : profile.filtering
        )
#else
        if let frame = frameStore.frame,
           let image = frame.image ?? frame.makeCGImage() {
            Image(decorative: image, scale: 1, orientation: .up)
                .resizable()
                .interpolation(profile.filtering ? .high : .none)
        }
#endif
    }

    private func pointerGesture(
        frameSize: CGSize,
        availableSize: CGSize
    ) -> some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                guard profile.touchInput else { return }
                let action: Int32 = pointerIsDown ? 3 : 1
                guard let point = pointerPoint(
                    value.location,
                    frameSize: frameSize,
                    availableSize: availableSize,
                    clampOutside: pointerIsDown
                ) else {
                    return
                }

                pointerIsDown = true
                session.sendPointer(x: point.x, y: point.y, action: action)
            }
            .onEnded { value in
                defer { pointerIsDown = false }
                guard profile.touchInput,
                      pointerIsDown,
                      let point = pointerPoint(
                        value.location,
                        frameSize: frameSize,
                        availableSize: availableSize,
                        clampOutside: true
                      ) else {
                    return
                }

                session.sendPointer(x: point.x, y: point.y, action: 2)
            }
    }

    private func pointerPoint(
        _ location: CGPoint,
        frameSize: CGSize,
        availableSize: CGSize,
        clampOutside: Bool
    ) -> (x: Int32, y: Int32)? {
        let rect = FrameLayout.renderedFrameRect(
            frameSize: frameSize,
            availableSize: availableSize,
            profile: profile,
            strictFit: fitsEntireFrame,
            forceFit: forcesNearestNeighborFit
        )
        guard rect.width > 0, rect.height > 0 else { return nil }
        guard clampOutside || rect.contains(location) else { return nil }

        let clampedX = min(max(location.x, rect.minX), rect.maxX)
        let clampedY = min(max(location.y, rect.minY), rect.maxY)
        let normalizedX = (clampedX - rect.minX) / rect.width
        let normalizedY = (clampedY - rect.minY) / rect.height
        let frameWidth = max(Int(frameSize.width.rounded()), 1)
        let frameHeight = max(Int(frameSize.height.rounded()), 1)
        let x = min(max(Int(normalizedX * CGFloat(frameWidth)), 0), frameWidth - 1)
        let y = min(max(Int(normalizedY * CGFloat(frameHeight)), 0), frameHeight - 1)
        return (Int32(x), Int32(y))
    }

}

private enum FrameLayout {
    static func renderedFrameRect(
        frame: PhoneMEFrame?,
        availableSize: CGSize,
        profile: GameProfile,
        strictFit: Bool = false,
        forceFit: Bool = false
    ) -> CGRect {
        let frameSize = frame.map {
            CGSize(width: $0.width, height: $0.height)
        } ?? CGSize(
            width: max(profile.screenWidth, 1),
            height: max(profile.screenHeight, 1)
        )
        return renderedFrameRect(
            frameSize: frameSize,
            availableSize: availableSize,
            profile: profile,
            strictFit: strictFit,
            forceFit: forceFit
        )
    }

    static func renderedFrameRect(
        frame: PhoneMEFrame,
        availableSize: CGSize,
        profile: GameProfile,
        strictFit: Bool = false,
        forceFit: Bool = false
    ) -> CGRect {
        renderedFrameRect(
            frameSize: CGSize(width: frame.width, height: frame.height),
            availableSize: availableSize,
            profile: profile,
            strictFit: strictFit,
            forceFit: forceFit
        )
    }

    static func renderedFrameRect(
        frameSize: CGSize,
        availableSize: CGSize,
        profile: GameProfile,
        strictFit: Bool,
        forceFit: Bool
    ) -> CGRect {
        let renderedSize: CGSize

        if forceFit {
            if profile.preserveAspectRatio {
                let fitScale = min(
                    availableSize.width / max(frameSize.width, 1),
                    availableSize.height / max(frameSize.height, 1)
                )
                renderedSize = CGSize(
                    width: frameSize.width * fitScale,
                    height: frameSize.height * fitScale
                )
            } else {
                renderedSize = availableSize
            }
        } else {
            switch profile.scaleType {
            case .asIs:
                let scale = CGFloat(profile.scalePercent) / 100
                renderedSize = CGSize(
                    width: frameSize.width * scale,
                    height: frameSize.height * scale
                )
            case .fit:
                let requestedScale = max(CGFloat(profile.scalePercent) / 100, 0.01)
                if profile.preserveAspectRatio {
                    let widthScale = availableSize.width / max(frameSize.width, 1)
                    let heightScale = availableSize.height / max(frameSize.height, 1)
                    let shouldFitWidth = !strictFit
                        && profile.screenGravity == .top
                        && frameSize.height >= frameSize.width
                        && availableSize.height >= availableSize.width
                    let fitScale = shouldFitWidth
                        ? widthScale
                        : min(widthScale, heightScale)
                    let scale = fitScale * requestedScale
                    renderedSize = CGSize(
                        width: frameSize.width * scale,
                        height: frameSize.height * scale
                    )
                } else {
                    renderedSize = CGSize(
                        width: availableSize.width * requestedScale,
                        height: availableSize.height * requestedScale
                    )
                }
            case .fill:
                if profile.preserveAspectRatio {
                    let fillScale = max(
                        availableSize.width / max(frameSize.width, 1),
                        availableSize.height / max(frameSize.height, 1)
                    )
                    renderedSize = CGSize(
                        width: frameSize.width * fillScale,
                        height: frameSize.height * fillScale
                    )
                } else {
                    renderedSize = availableSize
                }
            }
        }

        return CGRect(
            origin: alignedOrigin(
                renderedSize: renderedSize,
                availableSize: availableSize,
                gravity: profile.screenGravity
            ),
            size: renderedSize
        )
    }

    private static func alignedOrigin(
        renderedSize: CGSize,
        availableSize: CGSize,
        gravity: GameProfile.ScreenGravity
    ) -> CGPoint {
        switch gravity {
        case .left:
            return CGPoint(
                x: 0,
                y: (availableSize.height - renderedSize.height) / 2
            )
        case .top:
            return CGPoint(
                x: (availableSize.width - renderedSize.width) / 2,
                y: 0
            )
        case .center:
            return CGPoint(
                x: (availableSize.width - renderedSize.width) / 2,
                y: (availableSize.height - renderedSize.height) / 2
            )
        case .right:
            return CGPoint(
                x: availableSize.width - renderedSize.width,
                y: (availableSize.height - renderedSize.height) / 2
            )
        case .bottom:
            return CGPoint(
                x: (availableSize.width - renderedSize.width) / 2,
                y: availableSize.height - renderedSize.height
            )
        }
    }
}

#if canImport(UIKit)
@MainActor
private struct PhoneMEFrameLayerView: UIViewRepresentable {
    let frameStore: EmulatorFrameStore
    let filtering: Bool

    func makeCoordinator() -> Coordinator {
        Coordinator(frameStore: frameStore)
    }

    func makeUIView(context: Context) -> PhoneMEFrameLayerHostView {
        let view = PhoneMEFrameLayerHostView()
        context.coordinator.attach(view: view, filtering: filtering)
        return view
    }

    func updateUIView(
        _ uiView: PhoneMEFrameLayerHostView,
        context: Context
    ) {
        context.coordinator.update(
            frameStore: frameStore,
            filtering: filtering
        )
    }

    static func dismantleUIView(
        _ uiView: PhoneMEFrameLayerHostView,
        coordinator: Coordinator
    ) {
        coordinator.detach()
        uiView.clearFrame()
    }

    @MainActor
    final class Coordinator {
        private var frameStore: EmulatorFrameStore
        private weak var view: PhoneMEFrameLayerHostView?
        private var observerIdentifier: UUID?
        private var filtering = false

        init(frameStore: EmulatorFrameStore) {
            self.frameStore = frameStore
        }

        func attach(view: PhoneMEFrameLayerHostView, filtering: Bool) {
            stopObserving()
            self.view = view
            self.filtering = filtering
            startObserving()
        }

        func update(
            frameStore: EmulatorFrameStore,
            filtering: Bool
        ) {
            let filteringChanged = self.filtering != filtering
            self.filtering = filtering
            if self.frameStore !== frameStore {
                stopObserving()
                self.frameStore = frameStore
                startObserving()
                return
            }
            if filteringChanged {
                present(frameStore.frame)
            }
        }

        func detach() {
            stopObserving()
            view = nil
        }

        private func startObserving() {
            observerIdentifier = frameStore.observeFrames { [weak self] frame in
                self?.present(frame)
            }
        }

        private func stopObserving() {
            if let observerIdentifier {
                frameStore.removeFrameObserver(observerIdentifier)
            }
            observerIdentifier = nil
        }

        private func present(_ frame: PhoneMEFrame?) {
            guard let view else { return }
            if let frame {
                view.update(frame: frame, filtering: filtering)
            } else {
                view.clearFrame()
            }
        }
    }
}

private struct PhoneMEHardwareKeyboardView: UIViewRepresentable {
    let onKey: (J2MEKey, Bool) -> Void

    func makeUIView(context: Context) -> PhoneMEHardwareKeyboardHostView {
        let view = PhoneMEHardwareKeyboardHostView()
        view.onKey = onKey
        return view
    }

    func updateUIView(
        _ uiView: PhoneMEHardwareKeyboardHostView,
        context: Context
    ) {
        uiView.onKey = onKey
        uiView.requestFocusIfNeeded()
    }

    static func dismantleUIView(
        _ uiView: PhoneMEHardwareKeyboardHostView,
        coordinator: Void
    ) {
        uiView.releaseAllKeys()
    }
}

private final class PhoneMEHardwareKeyboardHostView: UIView {
    var onKey: ((J2MEKey, Bool) -> Void)?

    private let suppressedSoftwareKeyboard = UIView(frame: .zero)
    private var heldKeyCodes = Set<Int32>()
    private var focusRequestPending = false

    override var canBecomeFirstResponder: Bool { true }

    // This responder exists only for UIPress events from a connected hardware
    // keyboard. Supplying an empty input view prevents UIKit from repeatedly
    // constructing/dismissing the software keyboard while the game is active.
    override var inputView: UIView? { suppressedSoftwareKeyboard }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil {
            focusRequestPending = false
            releaseAllKeys()
        } else {
            requestFocusIfNeeded()
        }
    }

    override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
        false
    }

    func requestFocusIfNeeded() {
        guard window != nil, !isFirstResponder, !focusRequestPending else {
            return
        }
        focusRequestPending = true
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.focusRequestPending = false
            guard self.window != nil, !self.isFirstResponder else { return }
            _ = self.becomeFirstResponder()
        }
    }

    override func pressesBegan(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        var handled = false
        for press in presses {
            guard let uiKey = press.key,
                  let key = Self.j2meKey(for: uiKey) else {
                continue
            }
            handled = true
            if heldKeyCodes.insert(key.rawValue).inserted {
                onKey?(key, true)
            }
        }
        if !handled {
            super.pressesBegan(presses, with: event)
        }
    }

    override func pressesEnded(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        var handled = false
        for press in presses {
            guard let uiKey = press.key,
                  let key = Self.j2meKey(for: uiKey) else {
                continue
            }
            handled = true
            if heldKeyCodes.remove(key.rawValue) != nil {
                onKey?(key, false)
            }
        }
        if !handled {
            super.pressesEnded(presses, with: event)
        }
    }

    override func pressesCancelled(
        _ presses: Set<UIPress>,
        with event: UIPressesEvent?
    ) {
        for press in presses {
            guard let uiKey = press.key,
                  let key = Self.j2meKey(for: uiKey),
                  heldKeyCodes.remove(key.rawValue) != nil else {
                continue
            }
            onKey?(key, false)
        }
        super.pressesCancelled(presses, with: event)
    }

    func releaseAllKeys() {
        let keys = heldKeyCodes.compactMap(J2MEKey.init(rawValue:))
        heldKeyCodes.removeAll(keepingCapacity: true)
        for key in keys {
            onKey?(key, false)
        }
    }

    private static func j2meKey(for key: UIKey) -> J2MEKey? {
        switch key.charactersIgnoringModifiers.lowercased() {
        case UIKeyCommand.inputUpArrow, "w", "i": return .up
        case UIKeyCommand.inputDownArrow, "s", "k": return .down
        case UIKeyCommand.inputLeftArrow, "a", "j": return .left
        case UIKeyCommand.inputRightArrow, "d", "l": return .right
        case "\r", "\n", " ": return .fire
        case "q", "z", "[": return .softLeft
        case "e", "x", "]", "\u{1b}", "\u{8}", "\u{7f}": return .softRight
        case "0": return .zero
        case "1": return .one
        case "2": return .two
        case "3": return .three
        case "4": return .four
        case "5": return .five
        case "6": return .six
        case "7": return .seven
        case "8": return .eight
        case "9": return .nine
        case "*": return .star
        case "#": return .pound
        default: return nil
        }
    }
}

private final class PhoneMEFrameLayerHostView: MTKView, MTKViewDelegate {
    private static let shaderSource = """
    #include <metal_stdlib>
    using namespace metal;

    struct PhoneMEFrameVertex {
        float4 position [[position]];
        float2 texCoord;
    };

    vertex PhoneMEFrameVertex phoneMEFrameVertex(uint vertexID [[vertex_id]]) {
        constexpr float2 positions[] = {
            float2(-1.0, -1.0),
            float2( 3.0, -1.0),
            float2(-1.0,  3.0)
        };
        constexpr float2 texCoords[] = {
            float2(0.0, 1.0),
            float2(2.0, 1.0),
            float2(0.0, -1.0)
        };
        PhoneMEFrameVertex output;
        output.position = float4(positions[vertexID], 0.0, 1.0);
        output.texCoord = texCoords[vertexID];
        return output;
    }

    fragment float4 phoneMEFrameFragment(
        PhoneMEFrameVertex input [[stage_in]],
        texture2d<float> frameTexture [[texture(0)]],
        sampler frameSampler [[sampler(0)]]) {
        return frameTexture.sample(frameSampler, input.texCoord);
    }
    """

    private struct RendererResources {
        let deviceRegistryID: UInt64
        let commandQueue: MTLCommandQueue
        let pipelineState: MTLRenderPipelineState
        let nearestSampler: MTLSamplerState
        let linearSampler: MTLSamplerState
    }

    private static let rendererResourceLock = NSLock()
    private static var cachedRendererResources: RendererResources?
    private static var isCompilingRendererResources = false
    private static var rendererCompilationFailed = false

    static func prewarmRenderer() {
        guard let device = MTLCreateSystemDefaultDevice() else { return }
        requestRendererResources(for: device)
    }

    private let fallbackLayer = CALayer()
    private var commandQueue: MTLCommandQueue?
    private var pipelineState: MTLRenderPipelineState?
    private var nearestSampler: MTLSamplerState?
    private var linearSampler: MTLSamplerState?
    private var frameTexture: MTLTexture?
    private var currentFrame: PhoneMEFrame?
    private var usesLinearFiltering = false

    override init(frame: CGRect, device: MTLDevice?) {
        super.init(frame: frame, device: device ?? MTLCreateSystemDefaultDevice())
        configureRenderer()
    }

    required init(coder: NSCoder) {
        super.init(coder: coder)
        if device == nil {
            device = MTLCreateSystemDefaultDevice()
        }
        configureRenderer()
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        fallbackLayer.frame = bounds
    }

    func update(frame: PhoneMEFrame, filtering: Bool) {
        usesLinearFiltering = filtering
        layer.magnificationFilter = filtering ? .linear : .nearest
        layer.minificationFilter = filtering ? .linear : .nearest
        installRendererResourcesIfAvailable()
#if canImport(Metal)
        if let texture = frame.metalTexture,
           texture.device.registryID == device?.registryID,
           pipelineState != nil,
           commandQueue != nil,
           nearestSampler != nil,
           linearSampler != nil {
            currentFrame = frame
            frameTexture = texture
            fallbackLayer.isHidden = true
            setNeedsDisplay()
            return
        }
#endif
        currentFrame = frame
        guard let image = frame.image ?? frame.makeCGImage() else {
            clearFrame()
            return
        }
        guard upload(image: image) else {
            showFallback(image: image, filtering: filtering)
            return
        }
        fallbackLayer.isHidden = true
        setNeedsDisplay()
    }

    func clearFrame() {
        currentFrame = nil
        frameTexture = nil
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        fallbackLayer.contents = nil
        CATransaction.commit()
        setNeedsDisplay()
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
    }

    func draw(in view: MTKView) {
        guard
            let frameTexture,
            let commandQueue,
            let pipelineState,
            let renderPassDescriptor = currentRenderPassDescriptor,
            let drawable = currentDrawable,
            let commandBuffer = commandQueue.makeCommandBuffer(),
            let encoder = commandBuffer.makeRenderCommandEncoder(
                descriptor: renderPassDescriptor
            )
        else {
            return
        }

        encoder.setRenderPipelineState(pipelineState)
        encoder.setFragmentTexture(frameTexture, index: 0)
        encoder.setFragmentSamplerState(
            usesLinearFiltering ? linearSampler : nearestSampler,
            index: 0
        )
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
#if canImport(Metal)
        currentFrame?.retainMetalResources(until: commandBuffer)
#endif
        commandBuffer.present(drawable)
        commandBuffer.commit()
    }

    private func configureRenderer() {
        isOpaque = true
        isUserInteractionEnabled = false
        backgroundColor = .black
        // Keep the Metal drawable at the view's native backing resolution.
        // Rendering a 240x320-style guest framebuffer into a low-resolution
        // drawable and letting Core Animation magnify the CAMetalLayer makes
        // the final image visibly soft on Retina displays, even when the guest
        // texture itself uses nearest-neighbour sampling. Let Metal perform the
        // only upscale so filtering=false stays genuinely pixel-sharp.
        colorPixelFormat = .bgra8Unorm
        framebufferOnly = true
        autoResizeDrawable = true
        enableSetNeedsDisplay = true
        isPaused = true
        preferredFramesPerSecond = 60
        clearColor = MTLClearColorMake(0, 0, 0, 1)
        delegate = self

        fallbackLayer.frame = bounds
        fallbackLayer.contentsGravity = .resize
        fallbackLayer.backgroundColor = UIColor.black.cgColor
        layer.addSublayer(fallbackLayer)

        installRendererResourcesIfAvailable()
    }

    private func installRendererResourcesIfAvailable() {
        guard pipelineState == nil,
              let device,
              let resources = Self.rendererResourcesIfAvailable(for: device) else {
            return
        }
        commandQueue = resources.commandQueue
        pipelineState = resources.pipelineState
        nearestSampler = resources.nearestSampler
        linearSampler = resources.linearSampler
    }

    private static func rendererResourcesIfAvailable(
        for device: MTLDevice
    ) -> RendererResources? {
        rendererResourceLock.lock()
        if let cachedRendererResources,
           cachedRendererResources.deviceRegistryID == device.registryID {
            rendererResourceLock.unlock()
            return cachedRendererResources
        }
        let shouldCompile = !isCompilingRendererResources
            && !rendererCompilationFailed
        if shouldCompile {
            isCompilingRendererResources = true
        }
        rendererResourceLock.unlock()

        if shouldCompile {
            requestRendererResources(for: device, alreadyReserved: true)
        }
        return nil
    }

    private static func requestRendererResources(
        for device: MTLDevice,
        alreadyReserved: Bool = false
    ) {
        if !alreadyReserved {
            rendererResourceLock.lock()
            if let cachedRendererResources,
               cachedRendererResources.deviceRegistryID == device.registryID {
                rendererResourceLock.unlock()
                return
            }
            guard !isCompilingRendererResources,
                  !rendererCompilationFailed else {
                rendererResourceLock.unlock()
                return
            }
            isCompilingRendererResources = true
            rendererResourceLock.unlock()
        }

        DispatchQueue.global(qos: .userInitiated).async {
            let resources = buildRendererResources(for: device)
            rendererResourceLock.lock()
            cachedRendererResources = resources
            rendererCompilationFailed = resources == nil
            isCompilingRendererResources = false
            rendererResourceLock.unlock()
        }
    }

    private static func buildRendererResources(
        for device: MTLDevice
    ) -> RendererResources? {
        guard let commandQueue = device.makeCommandQueue() else { return nil }

        let nearest = MTLSamplerDescriptor()
        nearest.minFilter = .nearest
        nearest.magFilter = .nearest
        nearest.mipFilter = .notMipmapped
        nearest.sAddressMode = .clampToEdge
        nearest.tAddressMode = .clampToEdge
        guard let nearestSampler = device.makeSamplerState(descriptor: nearest) else {
            return nil
        }

        let linear = MTLSamplerDescriptor()
        linear.minFilter = .linear
        linear.magFilter = .linear
        linear.mipFilter = .notMipmapped
        linear.sAddressMode = .clampToEdge
        linear.tAddressMode = .clampToEdge
        guard let linearSampler = device.makeSamplerState(descriptor: linear) else {
            return nil
        }

        do {
            let library = try device.makeLibrary(
                source: shaderSource,
                options: nil
            )
            guard
                let vertex = library.makeFunction(name: "phoneMEFrameVertex"),
                let fragment = library.makeFunction(name: "phoneMEFrameFragment")
            else {
                return nil
            }
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = vertex
            descriptor.fragmentFunction = fragment
            descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
            let pipelineState = try device.makeRenderPipelineState(
                descriptor: descriptor
            )
            return RendererResources(
                deviceRegistryID: device.registryID,
                commandQueue: commandQueue,
                pipelineState: pipelineState,
                nearestSampler: nearestSampler,
                linearSampler: linearSampler
            )
        } catch {
            return nil
        }
    }

    private func upload(image: CGImage) -> Bool {
        guard
            let device,
            pipelineState != nil,
            commandQueue != nil,
            nearestSampler != nil,
            linearSampler != nil,
            image.width > 0,
            image.height > 0,
            let provider = image.dataProvider,
            let data = provider.data,
            let bytes = CFDataGetBytePtr(data)
        else {
            return false
        }

        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm,
            width: image.width,
            height: image.height,
            mipmapped: false
        )
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        if frameTexture?.width != image.width ||
            frameTexture?.height != image.height {
            frameTexture = device.makeTexture(descriptor: descriptor)
        }
        guard let frameTexture else { return false }
        frameTexture.replace(
            region: MTLRegionMake2D(0, 0, image.width, image.height),
            mipmapLevel: 0,
            withBytes: bytes,
            bytesPerRow: image.bytesPerRow
        )
        return true
    }

    private func showFallback(image: CGImage, filtering: Bool) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        fallbackLayer.isHidden = false
        fallbackLayer.magnificationFilter = filtering ? .linear : .nearest
        fallbackLayer.minificationFilter = filtering ? .linear : .nearest
        fallbackLayer.contents = image
        CATransaction.commit()
    }
}

// Starts Metal shader compilation while the library screen is idle. The game
// view never blocks the main thread waiting for a source shader compilation;
// until the shared pipeline is ready, it renders through the existing CALayer
// fallback and switches to Metal on a later frame.
enum PhoneMEFrameRendererPrewarmer {
    static func prewarm() {
        PhoneMEFrameLayerHostView.prewarmRenderer()
    }
}
#endif

private extension Color {
    static var playSurfaceBackground: Color {
#if canImport(UIKit)
        Color(uiColor: .systemGroupedBackground)
#elseif canImport(AppKit)
        Color(nsColor: .windowBackgroundColor)
#else
        Color.phoneMEAppBackground
#endif
    }
}
