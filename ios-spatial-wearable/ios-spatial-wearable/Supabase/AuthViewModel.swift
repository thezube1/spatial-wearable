import Foundation
import Observation
import Supabase

@Observable
final class AuthViewModel {
    var currentSession: Session?
    var isLoading = false
    var errorMessage: String?

    private let supabase = SupabaseService.shared.client

    init() {
        Task { await refreshSession() }
        Task { await observeAuth() }
    }

    func refreshSession() async {
        currentSession = try? await supabase.auth.session
    }

    private func observeAuth() async {
        for await change in supabase.auth.authStateChanges {
            await MainActor.run { self.currentSession = change.session }
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
