import SwiftUI
#if canImport(UIKit)
import UIKit
#endif

enum KeyboardAdjustmentMode: Equatable {
    case none
    case position
    case size
}

private enum KeyboardResizeAxis {
    case horizontal
    case vertical
}

private final class KeyboardGroupResizePreview: ObservableObject {
    struct Snapshot: Equatable {
        var isActive = false
        var widthRatio: CGFloat = 1
        var heightRatio: CGFloat = 1
    }

    @Published private(set) var snapshot = Snapshot()

    private var axis: KeyboardResizeAxis?
    private var targetScale: GameProfile.KeyboardGroupScale?

    func update(
        translation: CGSize,
        baseScale: GameProfile.KeyboardGroupScale,
        divisor: CGFloat,
        matchingScales: [Double]
    ) {
        if axis == nil {
            guard max(abs(translation.width), abs(translation.height)) >= 3 else {
                return
            }
            axis = abs(translation.width) >= abs(translation.height)
                ? .horizontal
                : .vertical
        }

        var result = baseScale
        switch axis {
        case .horizontal:
            result.width = snappedScale(
                baseScale.width + Double(translation.width / max(divisor, 1)),
                matchingScales: matchingScales
            )
        case .vertical:
            result.height = snappedScale(
                baseScale.height + Double(translation.height / max(divisor, 1)),
                matchingScales: matchingScales
            )
        case nil:
            return
        }

        targetScale = result
        let nextSnapshot = Snapshot(
            isActive: true,
            widthRatio: CGFloat(result.width / max(baseScale.width, 0.001)),
            heightRatio: CGFloat(result.height / max(baseScale.height, 0.001))
        )
        if snapshot != nextSnapshot {
            snapshot = nextSnapshot
        }
    }

    func finish() -> GameProfile.KeyboardGroupScale? {
        let result = targetScale
        reset()
        return result
    }

    func reset() {
        axis = nil
        targetScale = nil
        if snapshot != Snapshot() {
            snapshot = Snapshot()
        }
    }

    private func snappedScale(
        _ scale: Double,
        matchingScales: [Double]
    ) -> Double {
        let gridSize = 0.05
        let clamped = min(max(scale, 0.5), 1.8)
        let gridScale = (clamped / gridSize).rounded() * gridSize
        if abs(gridScale - 1) <= gridSize {
            return 1
        }
        if let match = matchingScales.first(where: {
            abs($0 - gridScale) <= gridSize / 2
        }) {
            return match
        }
        return gridScale
    }
}

private final class KeyboardEditPreviewStore: ObservableObject {
    let softKeys = KeyboardGroupResizePreview()
    let numbers = KeyboardGroupResizePreview()
    let directions = KeyboardGroupResizePreview()

    func preview(for groupID: String) -> KeyboardGroupResizePreview {
        switch groupID {
        case "soft-keys": return softKeys
        case "numbers": return numbers
        default: return directions
        }
    }

    func resetAll() {
        softKeys.reset()
        numbers.reset()
        directions.reset()
    }
}

@MainActor
private final class VirtualKeyboardControlPressState: ObservableObject {
    @Published var isPressed = false
}

@MainActor
private final class VirtualKeyboardInputCoordinator: ObservableObject {
    private var heldControls: [String: [J2MEKey]] = [:]
    private var keyOwnerCounts: [J2MEKey: Int] = [:]
    private var controlStates: [String: VirtualKeyboardControlPressState] = [:]
    private weak var activeSession: EmulatorSession?
#if canImport(UIKit)
    private let lightHaptic = UIImpactFeedbackGenerator(style: .light)
    private let mediumHaptic = UIImpactFeedbackGenerator(style: .medium)
#endif

    func state(for controlID: String) -> VirtualKeyboardControlPressState {
        if let state = controlStates[controlID] {
            return state
        }
        let state = VirtualKeyboardControlPressState()
        controlStates[controlID] = state
        return state
    }

    @discardableResult
    func press(
        controlID: String,
        keys: [J2MEKey],
        session: EmulatorSession
    ) -> Bool {
        guard heldControls[controlID] == nil else { return false }
        if let activeSession, activeSession !== session {
            _ = releaseAll(session: activeSession)
        }
        activeSession = session

        var seenKeys = Set<J2MEKey>()
        let uniqueKeys = keys.filter { seenKeys.insert($0).inserted }
        guard !uniqueKeys.isEmpty else { return false }

        heldControls[controlID] = uniqueKeys
        state(for: controlID).isPressed = true
        for key in uniqueKeys {
            let ownerCount = keyOwnerCounts[key, default: 0]
            keyOwnerCounts[key] = ownerCount + 1
            if ownerCount == 0 {
                session.send(key, pressed: true)
            }
        }
        return true
    }

    @discardableResult
    func release(
        controlID: String,
        session: EmulatorSession
    ) -> Bool {
        guard let keys = heldControls.removeValue(forKey: controlID) else {
            return false
        }
        state(for: controlID).isPressed = false
        let targetSession = activeSession ?? session

        for key in keys {
            guard let ownerCount = keyOwnerCounts[key] else { continue }
            if ownerCount <= 1 {
                keyOwnerCounts.removeValue(forKey: key)
                targetSession.send(key, pressed: false)
            } else {
                keyOwnerCounts[key] = ownerCount - 1
            }
        }
        if heldControls.isEmpty {
            activeSession = nil
        }
        return true
    }

#if canImport(UIKit)
    func performHaptic(emphasized: Bool) {
        let generator = emphasized ? mediumHaptic : lightHaptic
        generator.impactOccurred()
        generator.prepare()
    }
#endif

    @discardableResult
    func releaseAll(session: EmulatorSession) -> Int {
        let releasedControlCount = heldControls.count
        let targetSession = activeSession ?? session
        let heldControlIDs = Array(heldControls.keys)
        heldControls.removeAll(keepingCapacity: true)
        for controlID in heldControlIDs {
            state(for: controlID).isPressed = false
        }

        let keys = keyOwnerCounts.keys.sorted { $0.rawValue < $1.rawValue }
        keyOwnerCounts.removeAll(keepingCapacity: true)
        activeSession = nil
        for key in keys {
            targetSession.send(key, pressed: false)
        }
        return releasedControlCount
    }
}

struct KeyboardControlDescriptor: Identifiable, Equatable {
    let id: String
    let label: String
    let keys: [J2MEKey]
    let accessibilityLabel: String
    let emphasized: Bool
    let groupID: String
    let column: Int
    let row: Int
}

struct KeyboardLayoutDefinition {
    let metricColumns: Int
    let contentColumns: Int
    let rows: Int
    let keyHeightFactor: CGFloat
    let controls: [KeyboardControlDescriptor]
}

enum KeyboardLayoutCatalog {
    static func definition(for profile: GameProfile) -> KeyboardLayoutDefinition {
        definition(for: profile.resolvedKeyboardBaseType)
    }

    static func definition(
        for type: GameProfile.VirtualKeyboardType
    ) -> KeyboardLayoutDefinition {
        switch type == .custom ? .arrowsNumbers : type {
        case .numbersArrows:
            return split(numbersFirst: true)
        case .arrowsNumbers:
            return split(numbersFirst: false)
        case .phone:
            return phone(arrows: false)
        case .phoneArrows:
            return phone(arrows: true)
        case .numbers:
            return numbersOnly()
        case .arrows:
            return arrowsOnly()
        case .custom:
            return split(numbersFirst: false)
        }
    }

    static func selectableLayouts(
        hasCustomLayout: Bool
    ) -> [GameProfile.VirtualKeyboardType] {
        var layouts: [GameProfile.VirtualKeyboardType] = [
            .phone,
            .phoneArrows,
            .numbersArrows,
            .arrowsNumbers,
            .numbers,
            .arrows
        ]
        if hasCustomLayout {
            layouts.insert(.custom, at: 0)
        }
        return layouts
    }

    static func controlChoices(for profile: GameProfile) -> [KeyboardControlDescriptor] {
        definition(for: profile).controls
    }

    private static func split(numbersFirst: Bool) -> KeyboardLayoutDefinition {
        let numberStart = numbersFirst ? 0 : 3
        let directionStart = numbersFirst ? 3 : 0
        return KeyboardLayoutDefinition(
            metricColumns: 6,
            contentColumns: 6,
            rows: 4,
            keyHeightFactor: 0.75,
            controls: numberControls(startColumn: numberStart)
                + directionControls(startColumn: directionStart)
        )
    }

    private static func phone(arrows: Bool) -> KeyboardLayoutDefinition {
        let topMiddle = arrows
            ? descriptor(
                id: "menu",
                label: "M",
                keys: [],
                accessibilityLabel: L10n.string("Menu"),
                emphasized: false,
                groupID: "soft-keys",
                column: 1,
                row: 0
            )
            : key(.fire, label: "F", groupID: "soft-keys", column: 1, row: 0)

        var controls: [KeyboardControlDescriptor] = [
            key(.softLeft, groupID: "soft-keys", column: 0, row: 0),
            topMiddle,
            key(.softRight, groupID: "soft-keys", column: 2, row: 0)
        ]

        if arrows {
            controls += [
                key(.one, groupID: "numbers", column: 0, row: 1),
                key(.up, label: "↑", groupID: "directions", column: 1, row: 1),
                key(.three, groupID: "numbers", column: 2, row: 1),
                key(.left, label: "←", groupID: "directions", column: 0, row: 2),
                key(.fire, label: "F", groupID: "directions", column: 1, row: 2),
                key(.right, label: "→", groupID: "directions", column: 2, row: 2),
                key(.seven, groupID: "numbers", column: 0, row: 3),
                key(.down, label: "↓", groupID: "directions", column: 1, row: 3),
                key(.nine, groupID: "numbers", column: 2, row: 3),
                key(.star, groupID: "numbers", column: 0, row: 4),
                key(.zero, groupID: "numbers", column: 1, row: 4),
                key(.pound, groupID: "numbers", column: 2, row: 4)
            ]
        } else {
            let rows: [[J2MEKey]] = [
                [.one, .two, .three],
                [.four, .five, .six],
                [.seven, .eight, .nine],
                [.star, .zero, .pound]
            ]
            for (rowIndex, row) in rows.enumerated() {
                for (column, keyValue) in row.enumerated() {
                    controls.append(
                        key(
                            keyValue,
                            groupID: "numbers",
                            column: column,
                            row: rowIndex + 1
                        )
                    )
                }
            }
        }

        return KeyboardLayoutDefinition(
            metricColumns: 3,
            contentColumns: 3,
            rows: 5,
            keyHeightFactor: 0.5625,
            controls: controls
        )
    }

    private static func numbersOnly() -> KeyboardLayoutDefinition {
        var controls: [KeyboardControlDescriptor] = [
            key(.softLeft, groupID: "soft-keys", column: 0, row: 0),
            key(.softRight, groupID: "soft-keys", column: 4, row: 0)
        ]
        let rows: [[J2MEKey]] = [
            [.one, .two, .three],
            [.four, .five, .six],
            [.seven, .eight, .nine],
            [.star, .zero, .pound]
        ]
        for (rowIndex, row) in rows.enumerated() {
            for (index, keyValue) in row.enumerated() {
                controls.append(
                    key(
                        keyValue,
                        groupID: "numbers",
                        column: index + 1,
                        row: rowIndex
                    )
                )
            }
        }
        return KeyboardLayoutDefinition(
            metricColumns: 6,
            contentColumns: 5,
            rows: 4,
            keyHeightFactor: 0.75,
            controls: controls
        )
    }

    private static func arrowsOnly() -> KeyboardLayoutDefinition {
        let controls: [KeyboardControlDescriptor] = [
            key(.softLeft, groupID: "soft-keys", column: 0, row: 0),
            diagonal("↖", [.up, .left], id: "up-left", column: 1, row: 0),
            key(.up, label: "↑", groupID: "directions", column: 2, row: 0),
            diagonal("↗", [.up, .right], id: "up-right", column: 3, row: 0),
            key(.softRight, groupID: "soft-keys", column: 4, row: 0),
            key(.left, label: "←", groupID: "directions", column: 1, row: 1),
            key(.fire, label: "F", groupID: "directions", column: 2, row: 1),
            key(.right, label: "→", groupID: "directions", column: 3, row: 1),
            diagonal("↙", [.down, .left], id: "down-left", column: 1, row: 2),
            key(.down, label: "↓", groupID: "directions", column: 2, row: 2),
            diagonal("↘", [.down, .right], id: "down-right", column: 3, row: 2)
        ]
        return KeyboardLayoutDefinition(
            metricColumns: 6,
            contentColumns: 5,
            rows: 3,
            keyHeightFactor: 0.75,
            controls: controls
        )
    }

    private static func numberControls(startColumn: Int) -> [KeyboardControlDescriptor] {
        let rows: [[J2MEKey]] = [
            [.one, .two, .three],
            [.four, .five, .six],
            [.seven, .eight, .nine],
            [.star, .zero, .pound]
        ]
        return rows.enumerated().flatMap { rowIndex, row in
            row.enumerated().map { column, keyValue in
                key(
                    keyValue,
                    groupID: "numbers",
                    column: startColumn + column,
                    row: rowIndex
                )
            }
        }
    }

    private static func directionControls(startColumn: Int) -> [KeyboardControlDescriptor] {
        [
            key(.softLeft, groupID: "soft-keys", column: startColumn, row: 0),
            key(.softRight, groupID: "soft-keys", column: startColumn + 2, row: 0),
            diagonal("↖", [.up, .left], id: "up-left", column: startColumn, row: 1),
            key(.up, label: "↑", groupID: "directions", column: startColumn + 1, row: 1),
            diagonal("↗", [.up, .right], id: "up-right", column: startColumn + 2, row: 1),
            key(.left, label: "←", groupID: "directions", column: startColumn, row: 2),
            key(.fire, label: "F", groupID: "directions", column: startColumn + 1, row: 2),
            key(.right, label: "→", groupID: "directions", column: startColumn + 2, row: 2),
            diagonal("↙", [.down, .left], id: "down-left", column: startColumn, row: 3),
            key(.down, label: "↓", groupID: "directions", column: startColumn + 1, row: 3),
            diagonal("↘", [.down, .right], id: "down-right", column: startColumn + 2, row: 3)
        ]
    }

    private static func key(
        _ key: J2MEKey,
        label: String? = nil,
        groupID: String,
        column: Int,
        row: Int
    ) -> KeyboardControlDescriptor {
        descriptor(
            id: "key-\(key.rawValue)",
            label: label ?? key.title,
            keys: [key],
            accessibilityLabel: accessibilityLabel(for: key),
            emphasized: key == .fire,
            groupID: groupID,
            column: column,
            row: row
        )
    }

    private static func diagonal(
        _ label: String,
        _ keys: [J2MEKey],
        id: String,
        column: Int,
        row: Int
    ) -> KeyboardControlDescriptor {
        descriptor(
            id: id,
            label: label,
            keys: keys,
            accessibilityLabel: diagonalAccessibilityLabel(for: id),
            emphasized: false,
            groupID: "directions",
            column: column,
            row: row
        )
    }

    private static func descriptor(
        id: String,
        label: String,
        keys: [J2MEKey],
        accessibilityLabel: String,
        emphasized: Bool,
        groupID: String,
        column: Int,
        row: Int
    ) -> KeyboardControlDescriptor {
        KeyboardControlDescriptor(
            id: id,
            label: label,
            keys: keys,
            accessibilityLabel: accessibilityLabel,
            emphasized: emphasized,
            groupID: groupID,
            column: column,
            row: row
        )
    }

    private static func diagonalAccessibilityLabel(for id: String) -> String {
        switch id {
        case "up-left": return L10n.string("Up left")
        case "up-right": return L10n.string("Up right")
        case "down-left": return L10n.string("Down left")
        case "down-right": return L10n.string("Down right")
        default: return id.replacingOccurrences(of: "-", with: " ")
        }
    }

    private static func accessibilityLabel(for key: J2MEKey) -> String {
        switch key {
        case .up: return L10n.string("Up")
        case .down: return L10n.string("Down")
        case .left: return L10n.string("Left")
        case .right: return L10n.string("Right")
        case .fire: return L10n.string("Fire")
        case .softLeft: return L10n.string("Left soft key")
        case .softRight: return L10n.string("Right soft key")
        default: return key.title
        }
    }
}

struct KeypadView: View {
    @Environment(\.scenePhase) private var scenePhase
    @EnvironmentObject private var session: EmulatorSession

    @Binding var profile: GameProfile
    let editMode: KeyboardAdjustmentMode
    let layoutRect: CGRect
    let displayRect: CGRect
    let onKeyActivity: (Bool) -> Void
    let onObscuresDisplayChange: (Bool) -> Void

    // Keep live drag data outside the parent layout state. On iOS 16,
    // rebuilding the moving key hierarchy for every touch event can cancel
    // the active high-priority gesture and stall the main thread.
    @StateObject private var resizePreviews = KeyboardEditPreviewStore()
    @StateObject private var inputCoordinator = VirtualKeyboardInputCoordinator()
    @State private var inputResetGeneration: UInt = 0

    private let movementGridSize: CGFloat = 4

    var body: some View {
        GeometryReader { geometry in
            let definition = KeyboardLayoutCatalog.definition(for: profile)
            let customization = profile.effectiveKeyboardLayoutCustomization
            let matchingScales = customization.groupScales.values
                .flatMap { [$0.width, $0.height] }
            let frames = layoutFrames(
                definition: definition,
                size: geometry.size,
                layoutRect: layoutRect,
                customization: customization
            )
            let hidden = customization.hiddenControlIDs
            let visibleFrames = frames.filter { !hidden.contains($0.key) }
            let visibleControls = definition.controls.filter { !hidden.contains($0.id) }
            let controlsByID = Dictionary(
                uniqueKeysWithValues: visibleControls.map { ($0.id, $0) }
            )
            let obscuresDisplay = visibleFrames.values.contains { $0.intersects(displayRect) }

            ZStack(alignment: .topLeading) {
                ForEach(visibleControls) { control in
                    if let frame = frames[control.id] {
                        let groupScale = customization.groupScales[control.groupID]
                            ?? GameProfile.KeyboardGroupScale()
                        VirtualKeyButton(
                            control: control,
                            profile: profile,
                            editMode: editMode,
                            width: frame.width,
                            height: frame.height,
                            displayRect: displayRect,
                            obscuresDisplay: obscuresDisplay,
                            containerSize: geometry.size,
                            baseGroupScale: groupScale,
                            matchingGroupScales: matchingScales,
                            resizePreview: resizePreviews.preview(for: control.groupID),
                            movementGridSize: movementGridSize,
                            inputCoordinator: inputCoordinator,
                            pressState: inputCoordinator.state(for: control.id),
                            inputResetGeneration: inputResetGeneration,
                            onKeyActivity: onKeyActivity,
                            onPositionDragEnded: { translation in
                                commitControlPosition(
                                    control,
                                    translation: translation,
                                    definition: definition,
                                    size: geometry.size
                                )
                            },
                            onResizeDragEnded: { scale in
                                commitGroupScale(
                                    control.groupID,
                                    scale: scale
                                )
                            }
                        )
                        .environmentObject(session)
                        .position(x: frame.midX, y: frame.midY)
                    }
                }

#if canImport(UIKit)
                if editMode == .none {
                    VirtualKeyboardTouchRouterView(
                        targets: visibleControls.compactMap { control in
                            guard let frame = frames[control.id] else { return nil }
                            return VirtualKeyboardTouchTarget(
                                controlID: control.id,
                                frame: frame
                            )
                        },
                        resetGeneration: inputResetGeneration,
                        onControlPressed: { controlID in
                            guard let control = controlsByID[controlID] else { return }
                            pressVirtualControl(control)
                        },
                        onControlReleased: { controlID in
                            guard let control = controlsByID[controlID] else { return }
                            releaseVirtualControl(control)
                        }
                    )
                    .frame(width: geometry.size.width, height: geometry.size.height)
                }
#endif
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .onAppear {
                onObscuresDisplayChange(obscuresDisplay)
            }
            .onChange(of: editMode) { _ in
                resizePreviews.resetAll()
                releaseAllVirtualKeys()
            }
            .onChange(of: obscuresDisplay) { value in
                onObscuresDisplayChange(value)
            }
        }
        .onChange(of: scenePhase) { phase in
            if phase != .active {
                releaseAllVirtualKeys()
            }
        }
        .onChange(of: profile.virtualKeyboardType) { _ in
            releaseAllVirtualKeys()
        }
        .onDisappear {
            releaseAllVirtualKeys()
        }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Virtual keyboard")
    }

    private func releaseAllVirtualKeys() {
        let releasedControlCount = inputCoordinator.releaseAll(session: session)
        inputResetGeneration &+= 1
        for _ in 0..<releasedControlCount {
            onKeyActivity(false)
        }
    }

    private func pressVirtualControl(_ control: KeyboardControlDescriptor) {
        guard inputCoordinator.press(
            controlID: control.id,
            keys: control.keys,
            session: session
        ) else {
            return
        }
        onKeyActivity(true)
#if canImport(UIKit)
        if profile.hapticFeedback {
            inputCoordinator.performHaptic(emphasized: control.emphasized)
        }
#endif
    }

    private func releaseVirtualControl(_ control: KeyboardControlDescriptor) {
        guard inputCoordinator.release(
            controlID: control.id,
            session: session
        ) else {
            return
        }
        onKeyActivity(false)
    }

    private func layoutFrames(
        definition: KeyboardLayoutDefinition,
        size: CGSize,
        layoutRect: CGRect,
        customization: GameProfile.KeyboardLayoutCustomization
    ) -> [String: CGRect] {
        guard size.width > 0, size.height > 0 else { return [:] }

        let sizingSize = layoutRect.size
        let baseGap = min(8, max(4, sizingSize.width / 82))
        let gap = baseGap
        let unscaledWidth = max(
            28,
            (sizingSize.width - gap * CGFloat(definition.metricColumns - 1))
                / CGFloat(definition.metricColumns)
        )
        let availableHeight = max(
            28,
            (sizingSize.height - gap * CGFloat(definition.rows - 1))
                / CGFloat(definition.rows)
        )
        let keyWidth = unscaledWidth
        let keyHeight = min(
            unscaledWidth * definition.keyHeightFactor,
            availableHeight
        )
        let contentWidth = keyWidth * CGFloat(definition.contentColumns)
            + gap * CGFloat(definition.contentColumns - 1)
        let contentHeight = keyHeight * CGFloat(definition.rows)
            + gap * CGFloat(definition.rows - 1)
        let startX = layoutRect.minX + (layoutRect.width - contentWidth) / 2
        let startY = layoutRect.maxY - contentHeight

        var frames: [String: CGRect] = [:]
        for control in definition.controls {
            let groupScale = customization.groupScales[control.groupID]
                ?? GameProfile.KeyboardGroupScale()
            let offset = customization.controlOffsets[control.id]
                ?? GameProfile.KeyboardControlOffset()
            let columnOffset = CGFloat(control.column) * (keyWidth + gap)
            let rowOffset = CGFloat(control.row) * (keyHeight + gap)
            let centerX = startX + columnOffset + keyWidth / 2
                + CGFloat(offset.x) * size.width
            let centerY = startY + rowOffset + keyHeight / 2
                + CGFloat(offset.y) * size.height
            let scaledWidth = max(24, keyWidth * CGFloat(groupScale.width))
            let scaledHeight = max(18, keyHeight * CGFloat(groupScale.height))
            frames[control.id] = CGRect(
                x: centerX - scaledWidth / 2,
                y: centerY - scaledHeight / 2,
                width: scaledWidth,
                height: scaledHeight
            )
        }
        return frames
    }

    private func commitControlPosition(
        _ control: KeyboardControlDescriptor,
        translation: CGSize,
        definition: KeyboardLayoutDefinition,
        size: CGSize
    ) {
        guard size.width > 0, size.height > 0 else { return }

        let currentCustomization = profile.effectiveKeyboardLayoutCustomization
        let origin = currentCustomization.controlOffsets[control.id]
            ?? GameProfile.KeyboardControlOffset()
        let gridTranslation = CGSize(
            width: snapped(translation.width, step: movementGridSize),
            height: snapped(translation.height, step: movementGridSize)
        )
        var candidateOffset = GameProfile.KeyboardControlOffset(
            x: origin.x + Double(gridTranslation.width / size.width),
            y: origin.y + Double(gridTranslation.height / size.height)
        )
        var candidateCustomization = currentCustomization
        candidateCustomization.controlOffsets[control.id] = candidateOffset
        let candidateFrames = layoutFrames(
            definition: definition,
            size: size,
            layoutRect: layoutRect,
            customization: candidateCustomization
        )

        if let candidateFrame = candidateFrames[control.id] {
            let otherFrames = candidateFrames
                .filter {
                    $0.key != control.id
                        && !candidateCustomization.hiddenControlIDs.contains($0.key)
                }
                .map(\.value)
            let alignmentDelta = snapDelta(
                movingFrame: candidateFrame,
                otherFrames: otherFrames,
                containerSize: size
            )
            let alignedFrame = candidateFrame.offsetBy(
                dx: alignmentDelta.width,
                dy: alignmentDelta.height
            )
            let boundsDelta = containmentDelta(
                for: alignedFrame,
                in: CGRect(origin: .zero, size: size).insetBy(dx: 2, dy: 2)
            )
            candidateOffset.x += Double(
                (alignmentDelta.width + boundsDelta.width) / size.width
            )
            candidateOffset.y += Double(
                (alignmentDelta.height + boundsDelta.height) / size.height
            )
        }

        profile.updateKeyboardLayoutCustomization { customization in
            customization.controlOffsets[control.id] = candidateOffset.isDefault
                ? nil
                : candidateOffset
        }
    }

    private func commitGroupScale(
        _ groupID: String,
        scale: GameProfile.KeyboardGroupScale
    ) {
        profile.updateKeyboardLayoutCustomization { customization in
            customization.groupScales[groupID] = scale.isDefault ? nil : scale
        }
    }

    private func snapped(_ value: CGFloat, step: CGFloat) -> CGFloat {
        guard step > 0 else { return value }
        return (value / step).rounded() * step
    }

    private func containmentDelta(for frame: CGRect, in bounds: CGRect) -> CGSize {
        let horizontal: CGFloat
        if frame.width >= bounds.width {
            horizontal = bounds.midX - frame.midX
        } else if frame.minX < bounds.minX {
            horizontal = bounds.minX - frame.minX
        } else if frame.maxX > bounds.maxX {
            horizontal = bounds.maxX - frame.maxX
        } else {
            horizontal = 0
        }

        let vertical: CGFloat
        if frame.height >= bounds.height {
            vertical = bounds.midY - frame.midY
        } else if frame.minY < bounds.minY {
            vertical = bounds.minY - frame.minY
        } else if frame.maxY > bounds.maxY {
            vertical = bounds.maxY - frame.maxY
        } else {
            vertical = 0
        }
        return CGSize(width: horizontal, height: vertical)
    }

    private func snapDelta(
        movingFrame: CGRect,
        otherFrames: [CGRect],
        containerSize: CGSize
    ) -> CGSize {
        let radius: CGFloat = 9
        let movingX = [movingFrame.minX, movingFrame.midX, movingFrame.maxX]
        let movingY = [movingFrame.minY, movingFrame.midY, movingFrame.maxY]
        var targetX: [CGFloat] = [0, containerSize.width / 2, containerSize.width]
        var targetY: [CGFloat] = [0, containerSize.height / 2, containerSize.height]
        for frame in otherFrames {
            targetX += [frame.minX, frame.midX, frame.maxX]
            targetY += [frame.minY, frame.midY, frame.maxY]
        }
        return CGSize(
            width: closestSnapDelta(moving: movingX, targets: targetX, radius: radius),
            height: closestSnapDelta(moving: movingY, targets: targetY, radius: radius)
        )
    }

    private func closestSnapDelta(
        moving: [CGFloat],
        targets: [CGFloat],
        radius: CGFloat
    ) -> CGFloat {
        var best: CGFloat?
        for source in moving {
            for target in targets {
                let delta = target - source
                guard abs(delta) <= radius else { continue }
                if best == nil || abs(delta) < abs(best!) {
                    best = delta
                }
            }
        }
        return best ?? 0
    }
}

#if canImport(UIKit)
private struct VirtualKeyboardTouchTarget: Equatable {
    let controlID: String
    let frame: CGRect
}

private struct VirtualKeyboardTouchRouterView: UIViewRepresentable {
    let targets: [VirtualKeyboardTouchTarget]
    let resetGeneration: UInt
    let onControlPressed: (String) -> Void
    let onControlReleased: (String) -> Void

    func makeUIView(context: Context) -> TouchRouterView {
        let view = TouchRouterView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isAccessibilityElement = false
        view.isMultipleTouchEnabled = true
        view.isExclusiveTouch = false
        view.targets = targets
        view.onControlPressed = onControlPressed
        view.onControlReleased = onControlReleased
        view.applyResetGeneration(resetGeneration)
        return view
    }

    func updateUIView(_ uiView: TouchRouterView, context: Context) {
        uiView.targets = targets
        uiView.onControlPressed = onControlPressed
        uiView.onControlReleased = onControlReleased
        uiView.applyResetGeneration(resetGeneration)
    }

    final class TouchRouterView: UIView {
        var targets: [VirtualKeyboardTouchTarget] = []
        var onControlPressed: ((String) -> Void)?
        var onControlReleased: ((String) -> Void)?

        private var touchAssignments: [ObjectIdentifier: String] = [:]
        private var controlTouchCounts: [String: Int] = [:]
        private var controlPressStartedAt: [String: TimeInterval] = [:]
        private var pendingReleases: [String: DispatchWorkItem] = [:]
        private var resetGeneration: UInt?

        // Keep the visible layout untouched while making edge taps much more
        // forgiving. Candidate overlap is resolved by geometric distance, so
        // enlarging the interactive area does not create random key selection.
        private let minimumHitSlop: CGFloat = 4
        private let maximumHitSlop: CGFloat = 10
        private let moveHysteresis: CGFloat = 6
        private let minimumPressDuration: TimeInterval = 0.035

        func applyResetGeneration(_ generation: UInt) {
            guard resetGeneration != generation else { return }
            resetGeneration = generation
            for workItem in pendingReleases.values {
                workItem.cancel()
            }
            touchAssignments.removeAll(keepingCapacity: true)
            controlTouchCounts.removeAll(keepingCapacity: true)
            controlPressStartedAt.removeAll(keepingCapacity: true)
            pendingReleases.removeAll(keepingCapacity: true)
        }

        override func point(inside point: CGPoint, with event: UIEvent?) -> Bool {
            target(at: point) != nil
        }

        override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
            for touch in touches {
                let identifier = ObjectIdentifier(touch)
                guard touchAssignments[identifier] == nil,
                      let target = target(at: touch.location(in: self)) else {
                    continue
                }
                assign(identifier, to: target.controlID)
            }
        }

        override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
            for touch in touches {
                let identifier = ObjectIdentifier(touch)
                let location = touch.location(in: self)
                let currentControlID = touchAssignments[identifier]

                if let currentControlID,
                   let currentTarget = targets.first(where: {
                       $0.controlID == currentControlID
                   }),
                   interactiveFrame(for: currentTarget)
                       .insetBy(dx: -moveHysteresis, dy: -moveHysteresis)
                       .contains(location) {
                    continue
                }

                let nextControlID = target(at: location)?.controlID
                guard nextControlID != currentControlID else { continue }
                if currentControlID != nil {
                    // Switching to another key should be immediate. The short
                    // minimum-hold latch is only for tap-up, otherwise two
                    // directional keys can overlap briefly while sliding.
                    unassign(identifier, enforceMinimumDuration: false)
                }
                if let nextControlID {
                    assign(identifier, to: nextControlID)
                }
            }
        }

        override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
            finish(touches)
        }

        override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
            finish(touches)
        }

        override func didMoveToWindow() {
            super.didMoveToWindow()
            if window == nil {
                releaseEverythingImmediately()
            }
        }

        private func finish(_ touches: Set<UITouch>) {
            for touch in touches {
                unassign(ObjectIdentifier(touch))
            }
        }

        private func assign(_ touchID: ObjectIdentifier, to controlID: String) {
            touchAssignments[touchID] = controlID
            let count = controlTouchCounts[controlID, default: 0]
            controlTouchCounts[controlID] = count + 1
            if count == 0 {
                if let pendingRelease = pendingReleases.removeValue(forKey: controlID) {
                    pendingRelease.cancel()
                    controlPressStartedAt.removeValue(forKey: controlID)
                    onControlReleased?(controlID)
                }
                controlPressStartedAt[controlID] = ProcessInfo.processInfo.systemUptime
                onControlPressed?(controlID)
            }
        }

        private func unassign(
            _ touchID: ObjectIdentifier,
            enforceMinimumDuration: Bool = true
        ) {
            guard let controlID = touchAssignments.removeValue(forKey: touchID),
                  let count = controlTouchCounts[controlID] else {
                return
            }
            if count <= 1 {
                controlTouchCounts.removeValue(forKey: controlID)
                if enforceMinimumDuration {
                    scheduleRelease(for: controlID)
                } else {
                    releaseControlImmediately(controlID)
                }
            } else {
                controlTouchCounts[controlID] = count - 1
            }
        }

        private func scheduleRelease(for controlID: String) {
            let startedAt = controlPressStartedAt[controlID]
                ?? ProcessInfo.processInfo.systemUptime
            let elapsed = ProcessInfo.processInfo.systemUptime - startedAt
            let remaining = minimumPressDuration - elapsed
            guard remaining > 0 else {
                releaseControlImmediately(controlID)
                return
            }

            pendingReleases[controlID]?.cancel()
            let workItem = DispatchWorkItem { [weak self] in
                guard let self,
                      self.controlTouchCounts[controlID] == nil else {
                    return
                }
                self.pendingReleases.removeValue(forKey: controlID)
                self.releaseControlImmediately(controlID)
            }
            pendingReleases[controlID] = workItem
            DispatchQueue.main.asyncAfter(
                deadline: .now() + remaining,
                execute: workItem
            )
        }

        private func releaseControlImmediately(_ controlID: String) {
            pendingReleases.removeValue(forKey: controlID)?.cancel()
            controlPressStartedAt.removeValue(forKey: controlID)
            onControlReleased?(controlID)
        }

        private func releaseEverythingImmediately() {
            let heldControlIDs = Set(controlTouchCounts.keys)
                .union(pendingReleases.keys)
            guard !heldControlIDs.isEmpty else { return }

            for workItem in pendingReleases.values {
                workItem.cancel()
            }
            touchAssignments.removeAll(keepingCapacity: true)
            controlTouchCounts.removeAll(keepingCapacity: true)
            controlPressStartedAt.removeAll(keepingCapacity: true)
            pendingReleases.removeAll(keepingCapacity: true)
            for controlID in heldControlIDs {
                onControlReleased?(controlID)
            }
        }

        private func target(at point: CGPoint) -> VirtualKeyboardTouchTarget? {
            let exactTargets = targets.filter { $0.frame.contains(point) }
            if !exactTargets.isEmpty {
                return exactTargets.min {
                    squaredDistance(from: point, to: $0.frame.center)
                        < squaredDistance(from: point, to: $1.frame.center)
                }
            }

            return targets
                .filter { interactiveFrame(for: $0).contains(point) }
                .min {
                    let lhsDistance = squaredDistance(from: point, to: $0.frame)
                    let rhsDistance = squaredDistance(from: point, to: $1.frame)
                    if lhsDistance == rhsDistance {
                        return squaredDistance(from: point, to: $0.frame.center)
                            < squaredDistance(from: point, to: $1.frame.center)
                    }
                    return lhsDistance < rhsDistance
                }
        }

        private func interactiveFrame(
            for target: VirtualKeyboardTouchTarget
        ) -> CGRect {
            let horizontalSlop = hitSlop(for: target.frame.width)
            let verticalSlop = hitSlop(for: target.frame.height)
            return target.frame.insetBy(dx: -horizontalSlop, dy: -verticalSlop)
        }

        private func hitSlop(for dimension: CGFloat) -> CGFloat {
            let minimumTouchDimension: CGFloat = 44
            let sizeBasedSlop = max(0, (minimumTouchDimension - dimension) / 2)
            return min(max(sizeBasedSlop, minimumHitSlop), maximumHitSlop)
        }

        private func squaredDistance(from point: CGPoint, to center: CGPoint) -> CGFloat {
            let dx = point.x - center.x
            let dy = point.y - center.y
            return dx * dx + dy * dy
        }

        private func squaredDistance(from point: CGPoint, to rect: CGRect) -> CGFloat {
            let nearestX = min(max(point.x, rect.minX), rect.maxX)
            let nearestY = min(max(point.y, rect.minY), rect.maxY)
            let dx = point.x - nearestX
            let dy = point.y - nearestY
            return dx * dx + dy * dy
        }
    }
}

private extension CGRect {
    var center: CGPoint {
        CGPoint(x: midX, y: midY)
    }
}

private struct KeyboardEditPanGestureView: UIViewRepresentable {
    let onChanged: (CGSize) -> Void
    let onEnded: (CGSize) -> Void
    let onCancelled: () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(
            onChanged: onChanged,
            onEnded: onEnded,
            onCancelled: onCancelled
        )
    }

    func makeUIView(context: Context) -> UIView {
        let view = UIView(frame: .zero)
        view.backgroundColor = .clear
        view.isOpaque = false
        view.isAccessibilityElement = false

        let recognizer = UIPanGestureRecognizer(
            target: context.coordinator,
            action: #selector(Coordinator.handlePan(_:))
        )
        recognizer.minimumNumberOfTouches = 1
        recognizer.maximumNumberOfTouches = 1
        recognizer.cancelsTouchesInView = true
        view.addGestureRecognizer(recognizer)
        return view
    }

    func updateUIView(_ uiView: UIView, context: Context) {
        context.coordinator.onChanged = onChanged
        context.coordinator.onEnded = onEnded
        context.coordinator.onCancelled = onCancelled
    }

    final class Coordinator: NSObject {
        var onChanged: (CGSize) -> Void
        var onEnded: (CGSize) -> Void
        var onCancelled: () -> Void

        init(
            onChanged: @escaping (CGSize) -> Void,
            onEnded: @escaping (CGSize) -> Void,
            onCancelled: @escaping () -> Void
        ) {
            self.onChanged = onChanged
            self.onEnded = onEnded
            self.onCancelled = onCancelled
        }

        @objc func handlePan(_ recognizer: UIPanGestureRecognizer) {
            let translation = recognizer.translation(in: recognizer.view?.window)
            let size = CGSize(width: translation.x, height: translation.y)

            switch recognizer.state {
            case .began, .changed:
                onChanged(size)
            case .ended:
                onEnded(size)
            case .cancelled, .failed:
                onCancelled()
            default:
                break
            }
        }
    }
}
#endif

private struct VirtualKeyButton: View {
    @EnvironmentObject private var session: EmulatorSession
    @Environment(\.colorScheme) private var colorScheme

    let control: KeyboardControlDescriptor
    let profile: GameProfile
    let editMode: KeyboardAdjustmentMode
    let width: CGFloat
    let height: CGFloat
    let displayRect: CGRect
    let obscuresDisplay: Bool
    let containerSize: CGSize
    let baseGroupScale: GameProfile.KeyboardGroupScale
    let matchingGroupScales: [Double]
    @ObservedObject var resizePreview: KeyboardGroupResizePreview
    let movementGridSize: CGFloat
    let inputCoordinator: VirtualKeyboardInputCoordinator
    @ObservedObject var pressState: VirtualKeyboardControlPressState
    let inputResetGeneration: UInt
    let onKeyActivity: (Bool) -> Void
    let onPositionDragEnded: (CGSize) -> Void
    let onResizeDragEnded: (GameProfile.KeyboardGroupScale) -> Void

    @State private var isPressed = false
    @State private var editTranslation = CGSize.zero

    var body: some View {
        Group {
            if editMode == .none {
                GeometryReader { geometry in
                    keyContent(
                        effectiveOpacity: effectiveOpacity(
                            for: geometry.frame(in: .named("emulatorSurface"))
                        )
                    )
                }
            } else {
                // Editing always renders opaque controls and avoids one
                // GeometryReader per key while the drag gesture is active.
                keyContent(effectiveOpacity: 1)
            }
        }
        .frame(width: width, height: height)
        .scaleEffect(
            x: editMode == .size ? resizePreview.snapshot.widthRatio : 1,
            y: editMode == .size ? resizePreview.snapshot.heightRatio : 1,
            anchor: .center
        )
        .offset(positionPreviewTranslation)
        .onChange(of: inputResetGeneration) { _ in
            isPressed = false
        }
        .onDisappear {
            isPressed = false
            release()
        }
    }

    private func keyContent(effectiveOpacity: Double) -> some View {
        Text(control.label)
            .font(.system(size: min(width, height) * 0.36, weight: .medium))
            .foregroundStyle(labelColor)
            .frame(width: width, height: height)
            .background { buttonBackground(effectiveOpacity: effectiveOpacity) }
            .contentShape(Rectangle())
            .scaleEffect(effectivePressed && editMode == .none ? 0.96 : 1)
            .animation(.easeOut(duration: 0.06), value: effectivePressed)
            .overlay { keyboardInteractionLayer }
            .accessibilityLabel(control.accessibilityLabel)
            .accessibilityAddTraits(.isButton)
    }

    @ViewBuilder
    private var keyboardInteractionLayer: some View {
        if editMode == .none {
#if canImport(UIKit)
            // iOS uses one stable full-keyboard touch router above the keys.
            // Keeping the capture layer outside each animated key prevents a
            // press animation/layout update from disturbing the active touch.
            Color.clear
                .allowsHitTesting(false)
#else
            Color.clear
                .contentShape(Rectangle())
                .gesture(keyPressGesture)
#endif
        } else {
            editOutline
                .opacity(editMode == .size && !isGroupSelected ? 0.55 : 1)
            keyboardEditGestureLayer
        }
    }

#if !canImport(UIKit)
    private var keyPressGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { _ in
                setPressed(true)
            }
            .onEnded { _ in
                setPressed(false)
            }
    }
#endif

    private func setPressed(_ pressed: Bool) {
        guard isPressed != pressed else { return }
        isPressed = pressed
        if pressed {
            press()
        } else {
            release()
        }
    }

    @ViewBuilder
    private var keyboardEditGestureLayer: some View {
#if canImport(UIKit)
        // SwiftUI on iOS 15/16 can cancel a DragGesture when the same view is
        // offset during the drag. UIPanGestureRecognizer retains ownership of
        // the touch while SwiftUI redraws the key preview.
        KeyboardEditPanGestureView(
            onChanged: handleEditDragChanged,
            onEnded: handleEditDragEnded,
            onCancelled: cancelEditDrag
        )
#else
        Color.clear
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 3, coordinateSpace: .global)
                    .onChanged { value in
                        handleEditDragChanged(value.translation)
                    }
                    .onEnded { value in
                        handleEditDragEnded(value.translation)
                    }
            )
#endif
    }

    private var effectivePressed: Bool {
#if canImport(UIKit)
        pressState.isPressed
#else
        isPressed
#endif
    }

    private func handleEditDragChanged(_ translation: CGSize) {
        switch editMode {
        case .none:
            break
        case .position:
            editTranslation = translation
        case .size:
            resizePreview.update(
                translation: translation,
                baseScale: baseGroupScale,
                divisor: min(containerSize.width, containerSize.height),
                matchingScales: matchingGroupScales
            )
        }
    }

    private func handleEditDragEnded(_ translation: CGSize) {
        switch editMode {
        case .none:
            break
        case .position:
            editTranslation = .zero
            onPositionDragEnded(translation)
        case .size:
            if let scale = resizePreview.finish() {
                onResizeDragEnded(scale)
            }
        }
    }

    private func cancelEditDrag() {
        editTranslation = .zero
        resizePreview.reset()
    }

    private var positionPreviewTranslation: CGSize {
        guard editMode == .position else { return .zero }
        return CGSize(
            width: snapped(editTranslation.width, step: movementGridSize),
            height: snapped(editTranslation.height, step: movementGridSize)
        )
    }

    private var isGroupSelected: Bool {
        editMode == .size && resizePreview.snapshot.isActive
    }

    private func snapped(_ value: CGFloat, step: CGFloat) -> CGFloat {
        guard step > 0 else { return value }
        return (value / step).rounded() * step
    }

    private var editOutline: some View {
        RoundedRectangle(cornerRadius: 6, style: .continuous)
            .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, dash: [5, 3]))
            .padding(2)
    }

    @ViewBuilder
    private func buttonBackground(effectiveOpacity: Double) -> some View {
        switch profile.buttonShape {
        case .oval:
            styledBackground(Capsule(), effectiveOpacity: effectiveOpacity)
        case .rectangle:
            styledBackground(Rectangle(), effectiveOpacity: effectiveOpacity)
        case .roundedRectangle:
            styledBackground(
                RoundedRectangle(cornerRadius: min(width, height) * 0.24, style: .continuous),
                effectiveOpacity: effectiveOpacity
            )
        }
    }

    @ViewBuilder
    private func styledBackground<S: Shape>(
        _ shape: S,
        effectiveOpacity: Double
    ) -> some View {
        let selected = effectivePressed || isGroupSelected
        if profile.usesNativeKeyboardPalette {
            let neutralOverlay = Color.primary.opacity(colorScheme == .dark ? 0.10 : 0.04)
            let pressedFill = Color.accentColor.opacity(max(effectiveOpacity, 0.72))
            let outline = Color.primary.opacity(colorScheme == .dark ? 0.24 : 0.16)

            shape
                .fill(.thinMaterial)
                .opacity(effectiveOpacity)
                .overlay(shape.fill(selected ? pressedFill : neutralOverlay))
                .overlay(shape.stroke(outline, lineWidth: 0.75))
        } else {
            let fillHex = selected
                ? profile.keyboardSelectedBackgroundHex
                : profile.keyboardBackgroundHex
            let fill = (Color(j2meHex: fillHex) ?? .gray).opacity(effectiveOpacity)
            let outline = (Color(j2meHex: profile.keyboardOutlineHex) ?? .white)
                .opacity(effectiveOpacity)

            shape
                .fill(fill)
                .overlay(shape.stroke(outline, lineWidth: 1))
        }
    }

    private func effectiveOpacity(for keyFrame: CGRect) -> Double {
        if editMode != .none {
            return 1
        }
        guard obscuresDisplay else { return 1 }
        if profile.forceOpacityForOffscreenKeys, !keyFrame.intersects(displayRect) {
            return 1
        }
        return profile.keyboardOpacity
    }

    private var labelColor: Color {
        let selected = effectivePressed || isGroupSelected
        if profile.usesNativeKeyboardPalette {
            return selected ? .white : .primary
        }
        let hex = selected
            ? profile.keyboardSelectedForegroundHex
            : profile.keyboardForegroundHex
        return Color(j2meHex: hex) ?? .primary
    }

    private func press() {
        guard inputCoordinator.press(
            controlID: control.id,
            keys: control.keys,
            session: session
        ) else {
            return
        }
        onKeyActivity(true)
#if canImport(UIKit)
        if profile.hapticFeedback {
            inputCoordinator.performHaptic(emphasized: control.emphasized)
        }
#endif
    }

    private func release() {
        guard inputCoordinator.release(
            controlID: control.id,
            session: session
        ) else {
            return
        }
        onKeyActivity(false)
    }
}

private extension Color {
    init?(j2meHex: String) {
        let cleaned = j2meHex.trimmingCharacters(in: CharacterSet.alphanumerics.inverted)
        guard cleaned.count == 6, let value = UInt64(cleaned, radix: 16) else { return nil }
        self.init(
            red: Double((value >> 16) & 0xFF) / 255,
            green: Double((value >> 8) & 0xFF) / 255,
            blue: Double(value & 0xFF) / 255
        )
    }
}
