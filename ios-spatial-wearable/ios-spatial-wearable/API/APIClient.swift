import Foundation

enum APIError: Error, LocalizedError {
    case badURL
    case notAuthenticated
    case http(Int, String)
    case decoding(Error)

    var errorDescription: String? {
        switch self {
        case .badURL: "Bad URL"
        case .notAuthenticated: "Not signed in"
        case .http(let code, let msg): "HTTP \(code): \(msg)"
        case .decoding(let e): "Decode: \(e.localizedDescription)"
        }
    }
}

final class APIClient {
    static let shared = APIClient()

    private let baseURL: URL
    private let session: URLSession

    private init() {
        let info = Bundle.main.infoDictionary ?? [:]
        let raw = (info["FlaskAPIBaseURL"] as? String) ?? "http://localhost:5050"
        self.baseURL = URL(string: raw) ?? URL(string: "http://localhost:5050")!
        self.session = .shared
    }

    // MARK: - Core

    private func request<T: Decodable>(
        _ path: String,
        method: String = "GET",
        query: [URLQueryItem] = [],
        body: Encodable? = nil
    ) async throws -> T {
        let data = try await requestData(path, method: method, query: query, body: body)
        if T.self == EmptyResponse.self { return EmptyResponse() as! T }
        do { return try JSONDecoder().decode(T.self, from: data) }
        catch { throw APIError.decoding(error) }
    }

    @discardableResult
    private func requestData(
        _ path: String,
        method: String = "GET",
        query: [URLQueryItem] = [],
        body: Encodable? = nil
    ) async throws -> Data {
        var comps = URLComponents(
            url: baseURL.appendingPathComponent(path),
            resolvingAgainstBaseURL: false
        )
        if !query.isEmpty { comps?.queryItems = query }
        guard let url = comps?.url else { throw APIError.badURL }

        var req = URLRequest(url: url)
        req.httpMethod = method
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")

        guard let token = await SupabaseService.shared.accessToken() else {
            throw APIError.notAuthenticated
        }
        req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")

        if let body {
            req.httpBody = try JSONEncoder().encode(AnyEncodable(body))
        }

        let (data, resp) = try await session.data(for: req)
        guard let http = resp as? HTTPURLResponse else {
            throw APIError.http(0, "No response")
        }
        guard (200..<300).contains(http.statusCode) else {
            let msg = String(data: data, encoding: .utf8) ?? ""
            throw APIError.http(http.statusCode, msg)
        }
        return data
    }

    // MARK: - Endpoints

    func getMe() async throws -> UserProfile {
        try await request("/me")
    }

    func updateMe(username: String?, displayName: String?) async throws -> UserProfile {
        struct Body: Encodable { let username: String?; let display_name: String? }
        return try await request("/me", method: "PATCH",
                                 body: Body(username: username, display_name: displayName))
    }

    func searchUsers(query: String) async throws -> [UserProfile] {
        try await request("/users/search", query: [URLQueryItem(name: "q", value: query)])
    }

    func linkDevice(mac: String) async throws -> Device {
        struct Body: Encodable { let mac: String }
        return try await request("/devices/link", method: "POST", body: Body(mac: mac))
    }

    func getMyDevice() async throws -> Device? {
        let data = try await requestData("/devices/me")
        if data.isEmpty || (String(data: data, encoding: .utf8) == "null") { return nil }
        return try? JSONDecoder().decode(Device.self, from: data)
    }

    func listEvents() async throws -> [Event] {
        try await request("/events")
    }

    func getEvent(id: String) async throws -> Event {
        try await request("/events/\(id)")
    }

    func createGroup(name: String, eventId: String, memberIds: [String], leaderId: String?) async throws -> GroupDetail {
        struct Body: Encodable {
            let name: String
            let event_id: String
            let member_ids: [String]
            let leader_id: String?
        }
        return try await request("/groups", method: "POST",
                                 body: Body(name: name, event_id: eventId,
                                            member_ids: memberIds, leader_id: leaderId))
    }

    func getGroup(id: String) async throws -> GroupDetail {
        try await request("/groups/\(id)")
    }

    func addMember(groupId: String, userId: String) async throws {
        struct Body: Encodable { let user_id: String }
        _ = try await requestData("/groups/\(groupId)/members", method: "POST",
                                  body: Body(user_id: userId))
    }

    func removeMember(groupId: String, userId: String) async throws {
        _ = try await requestData("/groups/\(groupId)/members/\(userId)", method: "DELETE")
    }

    func setLeader(groupId: String, userId: String) async throws -> GroupDetail {
        struct Body: Encodable { let user_id: String }
        return try await request("/groups/\(groupId)/leader", method: "PATCH",
                                 body: Body(user_id: userId))
    }

    func joinGroup(joinCode: String) async throws -> GroupDetail {
        struct Body: Encodable { let join_code: String }
        return try await request("/groups/join", method: "POST", body: Body(join_code: joinCode))
    }
}

struct EmptyResponse: Decodable {}

private struct AnyEncodable: Encodable {
    let value: Encodable
    init(_ value: Encodable) { self.value = value }
    func encode(to encoder: Encoder) throws { try value.encode(to: encoder) }
}
