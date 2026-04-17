import Foundation
import Observation
import SwiftUI

enum OnboardingStep: String, CaseIterable {
    case welcome, linkWristband, selectEvent, createOrJoin, groupSetup, confirmation
}

@Observable
final class OnboardingCoordinator {
    var step: OnboardingStep {
        didSet { UserDefaults.standard.set(step.rawValue, forKey: "onboardingStep") }
    }

    // Wristband
    var linkedDeviceMAC: String?

    // Event
    var selectedEvent: Event?

    // Group
    var groupMode: GroupMode = .create
    var selectedMembers: [UserProfile] = []
    var groupName: String = ""
    var leaderId: String?
    var createdGroup: GroupDetail?
    var joinCode: String = ""

    // When true, SelectEventView's Continue routes back to groupSetup instead of createOrJoin.
    var returnToGroupSetupAfterEvent: Bool = false

    enum GroupMode { case create, join }

    init() {
        if let raw = UserDefaults.standard.string(forKey: "onboardingStep"),
           let s = OnboardingStep(rawValue: raw) {
            self.step = s
        } else {
            self.step = .welcome
        }
    }

    func advance() {
        let all = OnboardingStep.allCases
        guard let i = all.firstIndex(of: step), i + 1 < all.count else { return }
        step = all[i + 1]
    }

    func back() {
        let all = OnboardingStep.allCases
        guard let i = all.firstIndex(of: step), i > 0 else { return }
        step = all[i - 1]
    }

    func reset() {
        step = .welcome
        linkedDeviceMAC = nil
        selectedEvent = nil
        selectedMembers = []
        groupName = ""
        leaderId = nil
        createdGroup = nil
        joinCode = ""
    }
}
