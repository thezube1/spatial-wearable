import Foundation
// Developer: add the supabase-swift SPM dependency to this Xcode project:
//   https://github.com/supabase/supabase-swift
// Products used: `Supabase` (umbrella). This file assumes that package is
// linked; if it is missing the build will surface the unresolved import.
import Supabase
import Auth

enum SupabaseConfigError: Error { case missingKey(String) }

final class SupabaseService {
    static let shared = SupabaseService()

    let client: SupabaseClient

    private init() {
        let info = Bundle.main.infoDictionary ?? [:]
        guard
            let urlString = info["SUPABASE_URL"] as? String,
            let url = URL(string: urlString),
            let anonKey = info["SUPABASE_ANON_KEY"] as? String
        else {
            fatalError("Missing SUPABASE_URL or SUPABASE_ANON_KEY in Info.plist")
        }
        self.client = SupabaseClient(
            supabaseURL: url,
            supabaseKey: anonKey,
            options: SupabaseClientOptions(
                auth: SupabaseClientOptions.AuthOptions(
                    emitLocalSessionAsInitialSession: true
                )
            )
        )
    }

    /// Returns the current access token (JWT) if a session exists.
    func accessToken() async -> String? {
        do {
            let session = try await client.auth.session
            return session.accessToken
        } catch {
            return nil
        }
    }

    /// Returns the Supabase user_id (UUID string) of the current session, if any.
    func currentUserId() async -> String? {
        do {
            let session = try await client.auth.session
            return session.user.id.uuidString.lowercased()
        } catch {
            return nil
        }
    }
}
