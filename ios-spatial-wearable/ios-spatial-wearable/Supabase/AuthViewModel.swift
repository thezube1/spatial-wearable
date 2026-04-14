import Foundation
import Observation
import Supabase

@Observable
final class AuthViewModel {
    var currentSession: Session?
    var isLoading = false
    var errorMessage: String?

    private let supabase = SupabaseService.shared.client

    // UserDefaults flag — cleared when the app is deleted, so we can detect
    // fresh installs and flush any Keychain-persisted Supabase session.
    private static let firstLaunchKey = "hasLaunchedBefore"

    init() {
        Task {
            await clearStaleSessionOnFreshInstall()
            await refreshSession()
        }
        Task { await observeAuth() }
    }

    /// Supabase stores sessions in the iOS Keychain, which survives app
    /// deletion. On a first launch after (re)install we sign out so the user
    /// hits WelcomeView instead of being silently logged in.
    private func clearStaleSessionOnFreshInstall() async {
        let defaults = UserDefaults.standard
        if defaults.bool(forKey: Self.firstLaunchKey) { return }
        try? await supabase.auth.signOut()
        await MainActor.run { self.currentSession = nil }
        defaults.set(true, forKey: Self.firstLaunchKey)
    }

    func refreshSession() async {
        currentSession = try? await supabase.auth.session
    }

    private func observeAuth() async {
        for await change in supabase.auth.authStateChanges {
            let session = change.session
            let valid = session.map { !$0.isExpired } ?? false
            await MainActor.run { self.currentSession = valid ? session : nil }
        }
    }

    func signUp(email: String, password: String, username: String, displayName: String) async {
        isLoading = true; errorMessage = nil
        defer { isLoading = false }
        do {
            _ = try await supabase.auth.signUp(
                email: email,
                password: password,
                data: [
                    "username": .string(username),
                    "display_name": .string(displayName)
                ]
            )
            await refreshSession()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func signIn(email: String, password: String) async {
        isLoading = true; errorMessage = nil
        defer { isLoading = false }
        do {
            _ = try await supabase.auth.signIn(email: email, password: password)
            await refreshSession()
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    func signOut() async {
        try? await supabase.auth.signOut()
        currentSession = nil
    }
}
