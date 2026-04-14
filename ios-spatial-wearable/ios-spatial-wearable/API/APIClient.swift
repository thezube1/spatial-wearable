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
        let raw = (info["FlaskAPIBaseURL"] as? String) ?? "https://spatial-wearable-690270867902.us-west1.run.app"
        self.baseURL = URL(string: raw) ?? URL(string: "https://spatial-wearable-690270867902.us-west1.run.app")!
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
        struct Wrapper: Decodable { let user: UserProfile }
        let w: Wrapper = try await request("/me")
        return w.user
    }

    func updateMe(username: String?, displayName: String?) async throws -> UserProfile {
        struct Body: Encodable { let username: String?; let display_name: String? }
        struct Wrapper: Decodable { let user: UserProfile }
        let w: Wrapper = try await request("/me", method: "PATCH",
                                           body: Body(username: username, display_name: displayName))
        return w.user
    }

    func searchUsers(query: String) async throws -> [UserProfile] {
        struct Wrapper: Decodable { let users: [UserProfile] }
        let w: Wrapper = try await request("/users/search", query: [URLQueryItem(name: "q", value: query)])
        return w.users
    }

    func linkDevice(mac: String) async throws -> Device {
        struct Body: Encodable { let mac: String }
        struct Wrapper: Decodable { let device: Device }
        let w: Wrapper = try await request("/devices/link", method: "POST", body: Body(mac: mac))
        return w.device
    }

    func unlinkDevice() async throws {
        _ = try await requestData("/devices/me", method: "DELETE")
    }

    func getMyDevice() async throws -> Device? {
        struct Wrapper: Decodable { let device: Device? }
        let w: Wrapper = try await request("/devices/me")
        return w.device
    }

    func listEvents() async throws -> [Event] {
        struct Wrapper: Decodable { let events: [Event] }
        let w: Wrapper = try await request("/events")
        return w.events
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
        let resp: GroupResponse = try await request("/groups", method: "POST",
                                 body: Body(name: name, event_id: eventId,
                                            member_ids: memberIds, leader_id: leaderId))
        return resp.toDetail()
    }

    func getGroup(id: String) async throws -> GroupDetail {
        let resp: GroupResponse = try await request("/groups/\(id)")
        return resp.toDetail()
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
        _ = try await requestData("/groups/\(groupId)/leader", method: "PATCH",
                                  body: Body(user_id: userId))
        return try await getGroup(id: groupId)
    }

    func joinGroup(joinCode: String) async throws -> GroupDetail {
        struct Body: Encodable { let join_code: String }
        let resp: GroupResponse = try await request("/groups/join", method: "POST", body: Body(join_code: joinCode))
        return resp.toDetail()
    }
}

private struct GroupResponse: Decodable {
    struct Inner: Decodable { let id: String; let name: String; let join_code: String? }
    let group: Inner
    let members: [GroupMember]
    let leader: GroupMember?
    let event: Event?
    func toDetail() -> GroupDetail {
        GroupDetail(id: group.id, name: group.name, event: event,
                    leader: leader, members: members, join_code: group.join_code)
    }
}

struct EmptyResponse: Decodable {}

private struct AnyEncodable: Encodable {
    let value: Encodable
    init(_ value: Encodable) { self.value = value }
    func encode(to encoder: Encoder) throws { try value.encode(to: encoder) }
}
