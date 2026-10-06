#if os(visionOS)
import ARKit
import Foundation
import simd

// Stable order matches VIEW_HAND_JOINT_* in view.ns; no application gestures.
private let nsHandJoints: [HandSkeleton.JointName] = [
    .wrist,
    .thumbKnuckle,
    .thumbIntermediateBase,
    .thumbIntermediateTip,
    .thumbTip,
    .indexFingerMetacarpal,
    .indexFingerKnuckle,
    .indexFingerIntermediateBase,
    .indexFingerIntermediateTip,
    .indexFingerTip,
    .middleFingerMetacarpal,
    .middleFingerKnuckle,
    .middleFingerIntermediateBase,
    .middleFingerIntermediateTip,
    .middleFingerTip,
    .ringFingerMetacarpal,
    .ringFingerKnuckle,
    .ringFingerIntermediateBase,
    .ringFingerIntermediateTip,
    .ringFingerTip,
    .littleFingerMetacarpal,
    .littleFingerKnuckle,
    .littleFingerIntermediateBase,
    .littleFingerIntermediateTip,
    .littleFingerTip,
    .forearmWrist,
    .forearmArm
]

private struct NSHandSample {
    var tracked = false
    var timestamp = 0.0
    var jointsTracked = Array(repeating: false, count: 27)
    var positions = Array(repeating: SIMD3<Float>.zero, count: 27)
}

private final class NSHandMailbox: @unchecked Sendable {
    static let shared = NSHandMailbox()
    let lock = NSLock()
    var generation = 0
    var status: Int32 = 0
    var live = Array(repeating: NSHandSample(), count: 2)
    var snapshot = Array(repeating: NSHandSample(), count: 2)

    func clearSamples() {
        live = Array(repeating: NSHandSample(), count: 2)
        snapshot = live
    }
    func isCurrent(_ token: Int) -> Bool {
        lock.lock(); defer { lock.unlock() }
        return token == generation
    }
    func state(_ value: Int32, token: Int) {
        lock.lock(); defer { lock.unlock() }
        guard token == generation else { return }
        status = value
        live = Array(repeating: NSHandSample(), count: 2)
    }
    func publish(_ anchor: HandAnchor, removed: Bool, token: Int) {
        var sample = NSHandSample()
        sample.timestamp = ProcessInfo.processInfo.systemUptime
        if !removed, anchor.isTracked, let skeleton = anchor.handSkeleton {
            sample.tracked = true
            for (index, name) in nsHandJoints.enumerated() {
                let joint = skeleton.joint(name)
                sample.jointsTracked[index] = joint.isTracked
                if joint.isTracked {
                    let transform = anchor.originFromAnchorTransform * joint.anchorFromJointTransform
                    let p = transform.columns.3
                    sample.positions[index] = SIMD3(p.x, p.y, p.z)
                }
            }
        }
        lock.lock(); defer { lock.unlock() }
        guard token == generation, status == 2 else { return }
        live[anchor.chirality == .left ? 0 : 1] = sample
    }
}

@MainActor private final class NSHandSession {
    static let shared = NSHandSession()
    var updates: Task<Void, Never>?
    var events: Task<Void, Never>?
    var session: ARKitSession?
    func stop() {
        updates?.cancel(); updates = nil
        events?.cancel(); events = nil
        session?.stop(); session = nil
    }
    func fail(_ token: Int) {
        guard NSHandMailbox.shared.isCurrent(token) else { return }
        NSHandMailbox.shared.state(-1, token: token)
        stop()
    }
    func start(_ token: Int) {
        let mailbox = NSHandMailbox.shared
        guard mailbox.isCurrent(token) else { return }
        stop()
        let session = ARKitSession()
        self.session = session
        let provider = HandTrackingProvider()
        updates = Task {
            let result = await session.requestAuthorization(for: [.handTracking])
            guard !Task.isCancelled, mailbox.isCurrent(token) else { return }
            guard result[.handTracking] == .allowed else { fail(token); return }
            do {
                try await session.run([provider])
                guard !Task.isCancelled, mailbox.isCurrent(token) else { return }
                mailbox.state(2, token: token)
                events = Task {
                    for await event in session.events {
                        guard !Task.isCancelled, mailbox.isCurrent(token) else { break }
                        switch event {
                        case .authorizationChanged(let type, let status):
                            if type == .handTracking && status != .allowed { fail(token); return }
                        case .dataProviderStateChanged(_, let state, let error):
                            if error != nil || state == .stopped { fail(token); return }
                            mailbox.state(state == .running ? 2 : 1, token: token)
                        default: break
                        }
                    }
                }
                for await update in provider.anchorUpdates {
                    guard !Task.isCancelled, mailbox.isCurrent(token) else { break }
                    mailbox.publish(update.anchor, removed: update.event == .removed, token: token)
                }
            } catch {
                if !Task.isCancelled { fail(token) }
            }
        }
    }
}

@_cdecl("view_hands_start")
public func nsViewHandsStart() -> Int32 {
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock()
    if mailbox.status == 1 || mailbox.status == 2 {
        let status = mailbox.status
        mailbox.lock.unlock()
        return status
    }
    guard HandTrackingProvider.isSupported else {
        mailbox.status = -2
        mailbox.clearSamples()
        mailbox.lock.unlock()
        return -2
    }
    mailbox.generation += 1
    let token = mailbox.generation
    mailbox.status = 1
    mailbox.clearSamples()
    mailbox.lock.unlock()
    Task { @MainActor in NSHandSession.shared.start(token) }
    return 1
}

@_cdecl("view_hands_stop")
public func nsViewHandsStop() {
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock()
    mailbox.generation += 1
    let token = mailbox.generation
    mailbox.status = 0
    mailbox.clearSamples()
    mailbox.lock.unlock()
    Task { @MainActor in
        if mailbox.isCurrent(token) { NSHandSession.shared.stop() }
    }
}

@_cdecl("view_hands_snapshot")
public func nsViewHandsSnapshot() -> Int32 {
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock(); defer { mailbox.lock.unlock() }
    mailbox.snapshot = mailbox.live
    let now = ProcessInfo.processInfo.systemUptime
    for hand in 0..<2 {
        if mailbox.status != 2 || now - mailbox.snapshot[hand].timestamp > 0.20 {
            mailbox.snapshot[hand] = NSHandSample()
        }
    }
    return mailbox.status
}

@_cdecl("view_hand_tracked")
public func nsViewHandTracked(_ hand: Int32) -> Int32 {
    guard (0..<2).contains(hand) else { return 0 }
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock(); defer { mailbox.lock.unlock() }
    return mailbox.snapshot[Int(hand)].tracked ? 1 : 0
}

@_cdecl("view_hand_joint_tracked")
public func nsViewHandJointTracked(_ hand: Int32, _ joint: Int32) -> Int32 {
    guard (0..<2).contains(hand), (0..<27).contains(joint) else { return 0 }
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock(); defer { mailbox.lock.unlock() }
    let sample = mailbox.snapshot[Int(hand)]
    return sample.tracked && sample.jointsTracked[Int(joint)] ? 1 : 0
}

@_cdecl("view_hand_joint_position")
public func nsViewHandJointPosition(_ hand: Int32, _ joint: Int32, _ axis: Int32) -> Double {
    guard (0..<2).contains(hand), (0..<27).contains(joint), (0..<3).contains(axis) else { return 0 }
    let mailbox = NSHandMailbox.shared
    mailbox.lock.lock(); defer { mailbox.lock.unlock() }
    let sample = mailbox.snapshot[Int(hand)]
    guard sample.tracked, sample.jointsTracked[Int(joint)] else { return 0 }
    return Double(sample.positions[Int(joint)][Int(axis)])
}
#endif
