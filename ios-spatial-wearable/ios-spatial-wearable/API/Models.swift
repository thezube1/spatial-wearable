import Foundation

struct UserProfile: Codable, Identifiable, Hashable {
    let id: String
    let email: String?
    let username: String?
    let display_name: String?
    let avatar_url: String?
}

struct Device: Codable, Identifiable, Hashable {
    let id: String
    let mac_address: String
    let linked_user_id: String?
    let linked_at: String?
    let last_seen_at: String?
}

struct Event: Codable, Identifiable, Hashable {
    let id: String
    let name: String
    let venue: String?
    let city: String?
    let category: String?
    let starts_on: String?
    let ends_on: String?
    let image_url: String?
}

struct GroupMember: Codable, Identifiable, Hashable {
    var id: String { user_id }
    let user_id: String
    let username: String?
    let display_name: String?
    let avatar_url: String?
    let joined_at: String?
}

struct GroupSummary: Codable, Identifiable, Hashable {
    let id: String
    let name: String
    let event_id: String?
    let leader_id: String?
    let join_code: String?
}

struct GroupDetail: Codable, Identifiable, Hashable {
    let id: String
    let name: String
    let event: Event?
    let leader: GroupMember?
    let members: [GroupMember]
    let join_code: String?
}
