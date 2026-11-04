// Stellar Lord online server core.
//
// A tiny, dependency-free HTTP/1.1 server that hosts in-memory rift rooms.
// The same code powers the standalone stellar_lord_server.exe and the "host"
// option embedded in the desktop client.
//
//   GET  /api/ping
//   GET  /api/time
//   POST /api/room/create   id, name
//   POST /api/room/join     id, name, room
//   POST /api/room/leave    id, room
//   POST /api/room/start    id, room
//   POST /api/room/action   id, room, action, arg
//   GET  /api/room/state    id, room
//   GET  /api/room/list
//
// There is no account system and no disk persistence: the client generates a
// random player id locally and the server keeps every room in RAM only. Every
// player starts as a commoner when the host starts the match; all gameplay
// actions are validated and resolved by the server.

#include "server_core.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../src/game_model.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace rift_server {

constexpr int kDefaultPort = 8787;
constexpr size_t kMaxRequestBytes = 64 * 1024;
constexpr int kMaxConcurrentClients = 64;
constexpr int kClientTimeoutMs = 10000;
constexpr size_t kMaxRoomMembers = 8;
constexpr long long kRoomOfflineSeconds = 8;        // 未轮询即视为离线
constexpr long long kRoomDropWaitingSeconds = 120;  // 未开始的房间：清理掉线玩家
constexpr long long kRoomDropStartedSeconds = 900;  // 已开始的房间：保留更久
constexpr long long kRoomReconnectSeconds = 1800;   // 主动退出后保留存档，方便原 ID 回来
constexpr long long kRoomLifetimeSeconds = 6 * 3600;

// 全房间共享的审计压力。静默者不针对某个人，它针对整个裂谷。
constexpr double kAuditStart = 15.0;
constexpr double kAuditMax = 100.0;
constexpr double kAuditReliefDataCore = 15.0;
constexpr double kAuditReliefBroadcast = 5.0;
constexpr double kAuditBroadcastCost = 30.0;
constexpr double kAuditDataCoreCost = 40.0;

std::atomic<int> g_active_clients{0};
std::atomic<bool> g_running{false};
std::mutex g_log_mutex;
std::mutex g_room_mutex;
SOCKET g_listener = INVALID_SOCKET;
unsigned short g_port = 0;
std::atomic<bool> g_winsock_started{false};

// --- 小工具 ---------------------------------------------------------------

std::string Trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
                           text[begin] == '\n')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
                           text[end - 1] == '\n')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

std::string ToLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string ToUpper(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

void LogLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

std::string PercentDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]);
            const int low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        if (text[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string PercentEncode(std::string_view text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char ch : text) {
        const bool safe = std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~';
        if (safe) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0F]);
        }
    }
    return out;
}

std::map<std::string, std::string> ParseForm(std::string_view body) {
    std::map<std::string, std::string> fields;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t next = body.find('&', pos);
        if (next == std::string_view::npos) {
            next = body.size();
        }
        const std::string_view pair = body.substr(pos, next - pos);
        const size_t equal = pair.find('=');
        if (equal != std::string_view::npos) {
            fields[PercentDecode(pair.substr(0, equal))] = PercentDecode(pair.substr(equal + 1));
        }
        if (next == body.size()) {
            break;
        }
        pos = next + 1;
    }
    return fields;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

bool IsValidPlayerId(const std::string& id) {
    if (id.size() < 6 || id.size() > 32) {
        return false;
    }
    for (char ch : id) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool IsValidRoomCode(const std::string& code) {
    if (code.size() < 4 || code.size() > 12) {
        return false;
    }
    for (char ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

long long NowUnix() {
    return static_cast<long long>(std::time(nullptr));
}

// 昵称里不允许出现分隔符和控制字符，并保证是合法 UTF-8。
std::string CleanName(std::string name) {
    name = Trim(name);
    std::string cleaned;
    for (unsigned char ch : name) {
        if (ch == '|' || ch < 32 || ch == 127) {
            continue;
        }
        cleaned.push_back(static_cast<char>(ch));
        if (cleaned.size() >= 48) {
            break;
        }
    }
    while (!cleaned.empty()) {
        const int wide_length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, cleaned.data(),
                                                    static_cast<int>(cleaned.size()), nullptr, 0);
        if (wide_length > 0) {
            break;
        }
        cleaned.pop_back();
    }
    return cleaned.empty() ? "Lord" : cleaned;
}

std::string FormatDouble(double value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", std::isfinite(value) ? value : 0.0);
    return buffer;
}

// --- 房间模型 -------------------------------------------------------------

struct RoomMember {
    std::string id;
    std::string name;
    stellar::GameState state;
    long long last_seen{0};
    bool online{true};
    bool left{false};  // 主动退出：保留状态，允许同一玩家 ID 回来续档
};

struct Room {
    std::string code;
    std::string host_id;
    std::string host_name;
    bool started{false};
    long long created{0};
    long long started_ms{0};
    long long last_tick_ms{0};
    double audit{kAuditStart};
    int audit_tier{0};  // 0 未触发 / 1 采样 / 2 潮汐 / 3 归档
    std::vector<RoomMember> members;
};

void ApplyAuditCrisis(Room& room, int tier);
void RaiseAudit(Room& room, double amount);

std::map<std::string, Room> g_rooms;

int ActiveMemberCount(const Room& room) {
    int count = 0;
    for (const RoomMember& member : room.members) {
        if (!member.left) {
            ++count;
        }
    }
    return count;
}

RoomMember* FindMember(Room& room, const std::string& id) {
    for (RoomMember& member : room.members) {
        if (member.id == id) {
            return &member;
        }
    }
    return nullptr;
}

const RoomMember* FindMember(const Room& room, const std::string& id) {
    for (const RoomMember& member : room.members) {
        if (member.id == id) {
            return &member;
        }
    }
    return nullptr;
}

// 审计危机对所有成员同时生效。它不杀人，只归档。
void ApplyAuditCrisis(Room& room, int tier) {
    for (RoomMember& member : room.members) {
        stellar::GameState& state = member.state;
        switch (tier) {
        case 1:
            state.energy *= 0.92;
            state.minerals *= 0.92;
            state.data_cores += 1;  // 采样留下的一段可用数据
            state.morale = std::clamp(state.morale - 2.0, 0.0, 100.0);
            break;
        case 2:
            state.fleet = std::max(0, state.fleet - std::max(1, state.fleet / 10));
            state.food *= 0.88;
            state.morale = std::clamp(state.morale - 4.0, 0.0, 100.0);
            break;
        case 3:
            state.research *= 0.88;
            state.tokens *= 0.92;
            state.fleet = std::max(0, state.fleet - std::max(1, state.fleet / 12));
            state.morale = std::clamp(state.morale - 6.0, 0.0, 100.0);
            break;
        default:
            break;
        }
    }
    LogLine("room " + room.code + " audit crisis tier " + std::to_string(tier));
}

void RaiseAudit(Room& room, double amount) {
    if (!room.started || amount <= 0.0) {
        return;
    }
    room.audit = std::min(kAuditMax, room.audit + amount);
    const double thresholds[3] = {40.0, 65.0, 85.0};
    while (room.audit_tier < 3 && room.audit >= thresholds[room.audit_tier]) {
        ++room.audit_tier;
        ApplyAuditCrisis(room, room.audit_tier);
        room.audit = std::max(0.0, room.audit - 8.0);
    }
}

std::string GenerateRoomCode() {
    static const char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, static_cast<int>(sizeof(kAlphabet)) - 2);
    for (int attempt = 0; attempt < 200; ++attempt) {
        std::string code;
        for (int i = 0; i < 4; ++i) {
            code.push_back(kAlphabet[dist(rng)]);
        }
        if (!g_rooms.count(code)) {
            return code;
        }
    }
    return "RIFT";
}

// 玩家主动退出时不立刻删档：先标记 left/offline，同一 ID 在保留期内回来可以直接续档。
// 调用方必须持有 g_room_mutex。
void MarkMemberLeftLocked(std::map<std::string, Room>::iterator room_it, const std::string& id) {
    Room& room = room_it->second;
    RoomMember* member = FindMember(room, id);
    if (member != nullptr) {
        member->online = false;
        member->left = true;
        member->last_seen = NowUnix();
    }
    if (room.host_id == id) {
        room.host_id.clear();
        room.host_name.clear();
        for (const RoomMember& candidate : room.members) {
            if (candidate.id != id && !candidate.left) {
                room.host_id = candidate.id;
                room.host_name = candidate.name;
                break;
            }
        }
    }
}

// 调用方必须持有 g_room_mutex。
void LeaveOtherRoomsLocked(const std::string& id, const std::string& except_code = {}) {
    for (auto it = g_rooms.begin(); it != g_rooms.end();) {
        if (!except_code.empty() && it->first == except_code) {
            ++it;
            continue;
        }
        if (FindMember(it->second, id) != nullptr) {
            MarkMemberLeftLocked(it, id);
        }
        ++it;
    }
}

// 后台推进：只推进已经开始的房间。
void RoomTickLoop() {
    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::lock_guard<std::mutex> lock(g_room_mutex);
        const long long now = NowUnix();
        const long long now_ms = static_cast<long long>(GetTickCount64());
        for (auto it = g_rooms.begin(); it != g_rooms.end();) {
            Room& room = it->second;
            if (room.started) {
                double dt = static_cast<double>(now_ms - room.last_tick_ms) / 1000.0;
                if (dt < 0.0) {
                    dt = 0.0;
                }
                if (dt > 5.0) {
                    dt = 5.0;
                }
                if (dt > 0.0) {
                    for (RoomMember& member : room.members) {
                        stellar::Advance(member.state, dt);
                    }
                    room.last_tick_ms = now_ms;
                    const int active = std::max(1, ActiveMemberCount(room));
                    RaiseAudit(room, (0.002 + 0.0015 * static_cast<double>(active)) * dt);
                }
            }

            for (RoomMember& member : room.members) {
                member.online = !member.left && (now - member.last_seen) <= kRoomOfflineSeconds;
            }
            const long long drop_after =
                room.started ? kRoomDropStartedSeconds : kRoomDropWaitingSeconds;
            for (auto member_it = room.members.begin(); member_it != room.members.end();) {
                const long long grace =
                    member_it->left ? kRoomReconnectSeconds : drop_after;
                if (!member_it->online && now - member_it->last_seen > grace) {
                    member_it = room.members.erase(member_it);
                } else {
                    ++member_it;
                }
            }
            if (!room.members.empty() && FindMember(room, room.host_id) == nullptr) {
                room.host_id.clear();
                room.host_name.clear();
                for (const RoomMember& candidate : room.members) {
                    if (!candidate.left) {
                        room.host_id = candidate.id;
                        room.host_name = candidate.name;
                        break;
                    }
                }
            }
            if (room.members.empty() || now - room.created > kRoomLifetimeSeconds) {
                it = g_rooms.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// --- HTTP -----------------------------------------------------------------

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
    std::map<std::string, std::string> headers;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
};

std::string StatusText(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

std::string SerializeResponse(const HttpResponse& response) {
    std::string body = response.body;
    // 所有响应都带协议版本。客户端因此也能识别“旧服务端”，而不只是服务端拒绝旧客户端。
    const bool has_version_line =
        body.rfind("v=", 0) == 0 || body.find("\nv=") != std::string::npos;
    if (!has_version_line) {
        body += "v=" + std::to_string(kProtocolVersion) + "\n";
    }
    std::string out;
    out += "HTTP/1.1 " + std::to_string(response.status) + " " + StatusText(response.status) +
           "\r\n";
    out += "Content-Type: " + response.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "\r\n";
    out += body;
    return out;
}

bool SendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = ::send(socket, data.data() + sent,
                                 static_cast<int>(std::min<size_t>(data.size() - sent, 65536)), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

bool ParseRequestHead(const std::string& head, HttpRequest& request) {
    std::istringstream stream(head);
    std::string request_line;
    if (!std::getline(stream, request_line)) {
        return false;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }
    std::istringstream line_stream(request_line);
    std::string target;
    std::string version;
    if (!(line_stream >> request.method >> target >> version)) {
        return false;
    }
    const size_t question = target.find('?');
    if (question == std::string::npos) {
        request.path = target;
    } else {
        request.path = target.substr(0, question);
        request.query = target.substr(question + 1);
    }
    std::string header_line;
    while (std::getline(stream, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        if (header_line.empty()) {
            break;
        }
        const size_t colon = header_line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers[ToLower(Trim(header_line.substr(0, colon)))] =
            Trim(header_line.substr(colon + 1));
    }
    return true;
}

HttpResponse ErrorResponse(int status, const std::string& code) {
    HttpResponse response;
    response.status = status;
    response.body = "ok=0\nerr=" + code + "\n";
    return response;
}

bool HasCurrentVersion(const std::map<std::string, std::string>& fields) {
    const auto version = fields.find("v");
    return version != fields.end() && Trim(version->second) == std::to_string(kProtocolVersion);
}

HttpResponse VersionMismatchResponse() {
    return ErrorResponse(200, "version");
}

// --- 业务 -----------------------------------------------------------------

HttpResponse HandleRoomCreate(const HttpRequest& request) {
    const auto fields = ParseForm(request.body);
    if (!HasCurrentVersion(fields)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(fields.count("id") ? fields.at("id") : "");
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }
    const std::string name = CleanName(fields.count("name") ? fields.at("name") : "Lord");

    std::lock_guard<std::mutex> lock(g_room_mutex);
    LeaveOtherRoomsLocked(id);

    Room room;
    room.code = GenerateRoomCode();
    room.host_id = id;
    room.host_name = name;
    room.created = NowUnix();
    room.last_tick_ms = static_cast<long long>(GetTickCount64());

    RoomMember member;
    member.id = id;
    member.name = name;
    member.state = stellar::MakeNewGame(static_cast<double>(NowUnix()));
    member.last_seen = NowUnix();
    member.online = true;
    room.members.push_back(member);

    HttpResponse response;
    response.body = "ok=1\nroom=" + room.code + "\nhost=1\nhost_id=" + id +
                    "\nhost_name=" + name + "\nstarted=0\n";
    const std::string code = room.code;
    g_rooms[code] = std::move(room);
    LogLine("room " + code + " created by " + id);
    return response;
}

HttpResponse HandleRoomJoin(const HttpRequest& request) {
    const auto fields = ParseForm(request.body);
    if (!HasCurrentVersion(fields)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(fields.count("id") ? fields.at("id") : "");
    const std::string code = ToUpper(Trim(fields.count("room") ? fields.at("room") : ""));
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }
    if (!IsValidRoomCode(code)) {
        return ErrorResponse(400, "bad_room");
    }
    const std::string name = CleanName(fields.count("name") ? fields.at("name") : "Lord");
    LogLine("join attempt id=" + id + " room=[" + code + "] name=" + name);

    std::lock_guard<std::mutex> lock(g_room_mutex);
    auto room_it = g_rooms.find(code);
    if (room_it == g_rooms.end()) {
        LogLine("join rejected: no such room [" + code + "]");
        return ErrorResponse(200, "no_room");
    }
    LeaveOtherRoomsLocked(id, code);
    room_it = g_rooms.find(code);
    if (room_it == g_rooms.end()) {
        return ErrorResponse(200, "no_room");
    }
    Room& target = room_it->second;

    RoomMember* existing = FindMember(target, id);
    if (existing != nullptr) {
        existing->name = name;
        existing->last_seen = NowUnix();
        existing->online = true;
        existing->left = false;  // 原谅先离线的玩家：原 ID 回来继续原存档。
        if (target.host_id.empty()) {
            target.host_id = id;
            target.host_name = name;
        }
    } else {
        if (ActiveMemberCount(target) >= kMaxRoomMembers) {
            return ErrorResponse(200, "room_full");
        }
        RoomMember member;
        member.id = id;
        member.name = name;
        member.state = stellar::MakeNewGame(static_cast<double>(NowUnix()));
        member.left = false;
        if (target.started && target.started_ms > 0) {
            // 迟到加入：按已开局时间的 75% 追赶，最多补 30 分钟，避免完全跟不上。
            double elapsed = static_cast<double>(GetTickCount64() - target.started_ms) / 1000.0;
            elapsed = std::clamp(elapsed * 0.75, 0.0, 1800.0);
            if (elapsed > 1.0) {
                stellar::Advance(member.state, elapsed);
            }
        }
        member.last_seen = NowUnix();
        member.online = true;
        if (target.host_id.empty()) {
            target.host_id = id;
            target.host_name = name;
        }
        target.members.push_back(member);
    }

    HttpResponse response;
    response.body = "ok=1\nroom=" + target.code + "\nhost=" +
                    std::string(target.host_id == id ? "1" : "0") + "\nhost_id=" +
                    target.host_id + "\nhost_name=" + target.host_name +
                    "\nstarted=" + std::string(target.started ? "1" : "0") + "\n";
    LogLine("join ok room=" + target.code + " id=" + id + " members=" +
            std::to_string(target.members.size()));
    return response;
}

HttpResponse HandleRoomLeave(const HttpRequest& request) {
    const auto fields = ParseForm(request.body);
    if (!HasCurrentVersion(fields)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(fields.count("id") ? fields.at("id") : "");
    const std::string code = ToUpper(Trim(fields.count("room") ? fields.at("room") : ""));
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }

    std::lock_guard<std::mutex> lock(g_room_mutex);
    auto room_it = g_rooms.find(code);
    if (room_it != g_rooms.end()) {
        MarkMemberLeftLocked(room_it, id);
    }
    HttpResponse response;
    response.body = "ok=1\n";
    return response;
}

HttpResponse HandleRoomStart(const HttpRequest& request) {
    const auto fields = ParseForm(request.body);
    if (!HasCurrentVersion(fields)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(fields.count("id") ? fields.at("id") : "");
    const std::string code = ToUpper(Trim(fields.count("room") ? fields.at("room") : ""));
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }

    std::lock_guard<std::mutex> lock(g_room_mutex);
    auto room_it = g_rooms.find(code);
    if (room_it == g_rooms.end()) {
        return ErrorResponse(200, "no_room");
    }
    Room& room = room_it->second;
    if (room.host_id != id) {
        return ErrorResponse(200, "not_host");
    }
    if (room.members.empty()) {
        return ErrorResponse(200, "rejected");
    }
    if (room.started) {
        HttpResponse response;
        response.body = "ok=1\nroom=" + room.code + "\nstarted=1\n";
        return response;
    }

    const double now = static_cast<double>(NowUnix());
    for (RoomMember& member : room.members) {
        // 统一初始化成平民：开局前的一切都不带入对局。
        member.state = stellar::MakeNewGame(now);
        member.online = (NowUnix() - member.last_seen) <= kRoomOfflineSeconds;
    }
    room.started = true;
    room.started_ms = static_cast<long long>(GetTickCount64());
    room.last_tick_ms = room.started_ms;
    room.audit = kAuditStart;
    room.audit_tier = 0;

    HttpResponse response;
    response.body = "ok=1\nroom=" + room.code + "\nstarted=1\n";
    LogLine("room " + room.code + " started by " + id);
    return response;
}

std::string BattleBody(const stellar::BattleResult& result) {
    std::string body = "ok=1\nbattle=1\n";
    body += "won=" + std::string(result.won ? "1" : "0") + "\n";
    body += "captured=" + std::string(result.captured ? "1" : "0") + "\n";
    body += "loss=" + std::to_string(result.attacker_loss) + "\n";
    body += "defender_loss=" + std::to_string(result.defender_loss) + "\n";
    body += "loot_minerals=" + FormatDouble(result.loot_minerals) + "\n";
    body += "loot_alloy=" + FormatDouble(result.loot_alloy) + "\n";
    body += "loot_research=" + FormatDouble(result.loot_research) + "\n";
    body += "loot_tokens=" + FormatDouble(result.loot_tokens) + "\n";
    return body;
}

HttpResponse HandleRoomAction(const HttpRequest& request) {
    const auto fields = ParseForm(request.body);
    if (!HasCurrentVersion(fields)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(fields.count("id") ? fields.at("id") : "");
    const std::string code = ToUpper(Trim(fields.count("room") ? fields.at("room") : ""));
    const std::string action = Trim(fields.count("action") ? fields.at("action") : "");
    const std::string arg = Trim(fields.count("arg") ? fields.at("arg") : "");
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }
    if (!IsValidRoomCode(code)) {
        return ErrorResponse(400, "bad_room");
    }

    std::lock_guard<std::mutex> lock(g_room_mutex);
    auto room_it = g_rooms.find(code);
    if (room_it == g_rooms.end()) {
        return ErrorResponse(200, "no_room");
    }
    Room& room = room_it->second;
    RoomMember* member = FindMember(room, id);
    if (member == nullptr) {
        return ErrorResponse(200, "no_room");
    }
    member->last_seen = NowUnix();
    member->online = true;
    if (!room.started) {
        return ErrorResponse(200, "not_started");
    }

    const int index = std::atoi(arg.c_str());
    bool accepted = false;
    stellar::BattleResult battle;
    bool is_battle = false;
    double audit_delta = 0.0;

    if (action == "build") {
        if (index >= 0 && index < static_cast<int>(stellar::BuildingId::kCount)) {
            accepted = stellar::Build(member->state, static_cast<stellar::BuildingId>(index));
        }
    } else if (action == "capital") {
        accepted = stellar::UpgradeCapital(member->state);
    } else if (action == "research") {
        if (index >= 0 && index < static_cast<int>(stellar::TechId::kCount)) {
            accepted = stellar::BuyTech(member->state, static_cast<stellar::TechId>(index));
        }
    } else if (action == "fleet") {
        accepted = stellar::BuildFleet(member->state, std::clamp(index, 1, 100));
    } else if (action == "colonize") {
        accepted = stellar::Colonize(member->state);
    } else if (action == "colony") {
        accepted = stellar::UpgradeColony(member->state,
                                          static_cast<std::size_t>(std::max(0, index)));
    } else if (action == "colony_spec") {
        const size_t colon = arg.find(':');
        const int colony_index = std::max(0, std::atoi(arg.substr(0, colon).c_str()));
        if (colon == std::string::npos) {
            accepted = stellar::CycleColonySpecialization(
                member->state, static_cast<std::size_t>(colony_index));
        } else {
            const int spec = std::atoi(arg.substr(colon + 1).c_str());
            if (spec >= 0 &&
                spec < static_cast<int>(stellar::ColonySpecialization::kCount)) {
                accepted = stellar::SetColonySpecialization(
                    member->state, static_cast<std::size_t>(colony_index),
                    static_cast<stellar::ColonySpecialization>(spec));
            }
        }
    } else if (action == "faction") {
        accepted = stellar::ChooseFaction(member->state, index);
    } else if (action == "sell") {
        int resource = index;
        double amount = stellar::kTradeLotSize;
        const size_t colon = arg.find(':');
        if (colon != std::string::npos) {
            resource = std::atoi(arg.substr(0, colon).c_str());
            amount = std::atof(arg.substr(colon + 1).c_str());
        }
        accepted = stellar::SellResource(member->state, resource,
                                         std::clamp(amount, 1.0, 1000.0));
    } else if (action == "buy") {
        int resource = index;
        double amount = stellar::kTradeLotSize;
        const size_t colon = arg.find(':');
        if (colon != std::string::npos) {
            resource = std::atoi(arg.substr(0, colon).c_str());
            amount = std::atof(arg.substr(colon + 1).c_str());
        }
        accepted = stellar::BuyResource(member->state, resource,
                                        std::clamp(amount, 1.0, 1000.0));
    } else if (action == "contract") {
        accepted = stellar::FulfillContract(member->state,
                                            static_cast<std::size_t>(std::max(0, index)));
    } else if (action == "blackmarket") {
        accepted = stellar::BuyBlackMarket(member->state,
                                           static_cast<std::size_t>(std::max(0, index)));
        if (accepted) {
            audit_delta = 1.0;
        }
    } else if (action == "expedition") {
        const size_t colon = arg.find(':');
        if (colon != std::string::npos) {
            const int destination = std::atoi(arg.substr(0, colon).c_str());
            const int stance = std::atoi(arg.substr(colon + 1).c_str());
            if (destination >= 0 &&
                destination < static_cast<int>(stellar::ExpeditionDestination::kCount) &&
                stance >= 0 && stance < static_cast<int>(stellar::ExpeditionStance::kCount)) {
                accepted = stellar::LaunchExpedition(
                    member->state,
                    static_cast<stellar::ExpeditionDestination>(destination),
                    static_cast<stellar::ExpeditionStance>(stance));
                if (accepted) {
                    audit_delta = 0.6;
                }
            }
        }
    } else if (action == "clear_reports") {
        stellar::ClearExpeditionReports(member->state);
        accepted = true;
    } else if (action == "card") {
        accepted = stellar::ChooseCommandCard(member->state, index);
    } else if (action == "audit_relief") {
        if (room.audit > 0.5) {
            if (index == 0 && member->state.data_cores >= 1 &&
                member->state.energy >= kAuditDataCoreCost) {
                --member->state.data_cores;
                member->state.energy -= kAuditDataCoreCost;
                member->state.influence += 2.0;
                member->state.morale =
                    std::clamp(member->state.morale + 1.0, 0.0, 100.0);
                room.audit = std::max(0.0, room.audit - kAuditReliefDataCore);
                accepted = true;
            } else if (index == 1 && member->state.energy >= kAuditBroadcastCost) {
                member->state.energy -= kAuditBroadcastCost;
                member->state.influence += 1.0;
                room.audit = std::max(0.0, room.audit - kAuditReliefBroadcast);
                accepted = true;
            }
        }
    } else if (action == "wonder") {
        accepted = stellar::BuildWonder(member->state, index);
        if (accepted) {
            audit_delta = 4.0;
        }
    } else if (action == "resolve_event") {
        const size_t colon = arg.find(':');
        if (colon != std::string::npos) {
            const int event_index = std::atoi(arg.substr(0, colon).c_str());
            const bool choice_a = std::atoi(arg.substr(colon + 1).c_str()) != 0;
            accepted = event_index >= 0 &&
                       stellar::ResolveEvent(member->state,
                                             static_cast<std::size_t>(event_index), choice_a);
        }
    } else if (action == "attack_neutral") {
        if (member->state.fleet >= 5) {
            battle = stellar::AttackNeutral(member->state);
            is_battle = true;
            accepted = true;
            audit_delta = 1.0;
        }
    } else if (action == "attack_target") {
        if (index >= 0 && index < static_cast<int>(stellar::SectorTargetId::kCount) &&
            member->state.fleet >= 5) {
            battle = stellar::AttackSector(member->state,
                                           static_cast<stellar::SectorTargetId>(index));
            is_battle = true;
            accepted = true;
            audit_delta = 1.5;
        }
    } else if (action == "raid_player" || action == "attack_player") {
        RoomMember* target = FindMember(room, arg);
        if (target != nullptr && target->id != id && member->state.fleet >= 5) {
            battle = action == "raid_player" ? stellar::RaidPlayer(member->state, target->state)
                                             : stellar::AttackPlayer(member->state, target->state);
            is_battle = true;
            accepted = true;
            audit_delta = action == "raid_player" ? 3.0 : 4.0;
        }
    }

    if (!accepted) {
        return ErrorResponse(200, "rejected");
    }
    RaiseAudit(room, audit_delta);
    if (is_battle) {
        return HttpResponse{200, "text/plain; charset=utf-8", BattleBody(battle)};
    }
    HttpResponse response;
    response.body = "ok=1\n";
    return response;
}

HttpResponse HandleRoomState(const HttpRequest& request) {
    const auto query = ParseForm(request.query);
    if (!HasCurrentVersion(query)) {
        return VersionMismatchResponse();
    }
    const std::string id = Trim(query.count("id") ? query.at("id") : "");
    const std::string code = ToUpper(Trim(query.count("room") ? query.at("room") : ""));
    if (!IsValidPlayerId(id)) {
        return ErrorResponse(400, "bad_id");
    }

    std::lock_guard<std::mutex> lock(g_room_mutex);
    auto room_it = g_rooms.find(code);
    if (room_it == g_rooms.end()) {
        return ErrorResponse(200, "no_room");
    }
    Room& room = room_it->second;
    RoomMember* member = FindMember(room, id);
    if (member == nullptr) {
        return ErrorResponse(200, "no_room");
    }
    member->last_seen = NowUnix();
    member->online = true;

    const long long now = NowUnix();
    std::string body = "ok=1\n";
    body += "room=" + room.code + "\n";
    body += "started=" + std::string(room.started ? "1" : "0") + "\n";
    body += "host=" + std::string(room.host_id == id ? "1" : "0") + "\n";
    body += "host_id=" + room.host_id + "\n";
    body += "host_name=" + room.host_name + "\n";
    body += "audit=" + FormatDouble(room.audit) + "\n";
    body += "audit_tier=" + std::to_string(room.audit_tier) + "\n";
    body += "count=" + std::to_string(room.members.size()) + "\n";
    for (const RoomMember& other : room.members) {
        const bool is_online = other.online || (now - other.last_seen) <= kRoomOfflineSeconds;
        body += "member=" + other.id + "|" + other.name + "|" +
                FormatDouble(stellar::Score(other.state)) + "|" +
                std::to_string(other.state.fleet) + "|" +
                std::to_string(stellar::TotalColonyLevels(other.state)) + "|" +
                std::to_string(other.state.capital_level) + "|" +
                std::string(is_online ? "1" : "0") + "|" +
                std::string(other.id == id ? "1" : "0") + "|" +
                FormatDouble(other.state.tokens) + "|" +
                FormatDouble(other.state.influence) + "|" +
                std::to_string(other.state.relics) + "|" +
                std::to_string(other.state.cores) + "|" +
                std::to_string(other.state.data_cores) + "|" +
                std::to_string(other.state.reputation) + "\n";
    }
    if (room.started) {
        for (const std::string& line : SplitLines(stellar::SerializeState(member->state))) {
            body += "s=" + line + "\n";
        }
    }
    HttpResponse response;
    response.body = body;
    return response;
}

HttpResponse HandleRoomList(const HttpRequest& request) {
    const auto query = ParseForm(request.query);
    if (!HasCurrentVersion(query)) {
        return VersionMismatchResponse();
    }
    std::lock_guard<std::mutex> lock(g_room_mutex);
    std::string body = "ok=1\n";
    std::string rooms;
    int count = 0;
    for (const auto& pair : g_rooms) {
        const Room& room = pair.second;
        const int active = ActiveMemberCount(room);
        if (room.started || active <= 0) {
            continue;
        }
        rooms += "room=" + room.code + "|" + room.host_name + "|" +
                 std::to_string(active) + "|" +
                 std::string(room.started ? "1" : "0") + "\n";
        ++count;
    }
    body += "count=" + std::to_string(count) + "\n";
    body += rooms;
    HttpResponse response;
    response.body = body;
    return response;
}

HttpResponse HandleRequest(const HttpRequest& request) {
    if (request.method == "GET") {
        if (request.path == "/api/ping") {
            HttpResponse response;
            response.body = "ok=1\n";
            return response;
        }
        if (request.path == "/api/time") {
            HttpResponse response;
            response.body = "ok=1\ntime=" + std::to_string(NowUnix()) + "\n";
            return response;
        }
        if (request.path == "/api/room/state") {
            return HandleRoomState(request);
        }
        if (request.path == "/api/room/list") {
            return HandleRoomList(request);
        }
    } else if (request.method == "POST") {
        if (request.path == "/api/room/create") {
            return HandleRoomCreate(request);
        }
        if (request.path == "/api/room/join") {
            return HandleRoomJoin(request);
        }
        if (request.path == "/api/room/leave") {
            return HandleRoomLeave(request);
        }
        if (request.path == "/api/room/start") {
            return HandleRoomStart(request);
        }
        if (request.path == "/api/room/action") {
            return HandleRoomAction(request);
        }
    }

    return ErrorResponse(404, "not_found");
}

// --- 服务器循环 -----------------------------------------------------------

void ServeClient(SOCKET client) {
    DWORD timeout = kClientTimeoutMs;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));

    const ULONGLONG started = GetTickCount64();
    std::string buffer;
    buffer.reserve(8192);
    char chunk[8192];

    size_t header_end = std::string::npos;
    size_t content_length = 0;
    HttpRequest request;
    bool head_parsed = false;

    while (buffer.size() < kMaxRequestBytes) {
        const int received = ::recv(client, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            break;
        }
        buffer.append(chunk, static_cast<size_t>(received));
        if (!head_parsed) {
            header_end = buffer.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                continue;
            }
            if (!ParseRequestHead(buffer.substr(0, header_end), request)) {
                break;
            }
            head_parsed = true;
            const auto it = request.headers.find("content-length");
            if (it != request.headers.end()) {
                content_length = static_cast<size_t>(std::atoll(it->second.c_str()));
            }
            if (content_length > kMaxRequestBytes) {
                HttpResponse response = ErrorResponse(413, "too_large");
                SendAll(client, SerializeResponse(response));
                LogLine(request.method + " " + request.path + " -> 413 (too large)");
                return;
            }
        }
        if (head_parsed && buffer.size() >= header_end + 4 + content_length) {
            request.body = buffer.substr(header_end + 4, content_length);
            const HttpResponse response = HandleRequest(request);
            SendAll(client, SerializeResponse(response));
            LogLine(request.method + " " + request.path + " -> " + std::to_string(response.status) +
                    " (" + std::to_string(GetTickCount64() - started) + " ms)");
            return;
        }
    }

    if (!head_parsed) {
        HttpResponse response = ErrorResponse(400, "bad_request");
        SendAll(client, SerializeResponse(response));
        LogLine("malformed request -> 400");
    }
}

bool Start(unsigned short port, std::string* error_message) {
    if (g_running.load()) {
        if (error_message != nullptr) {
            *error_message = "server already running";
        }
        return false;
    }

    if (!g_winsock_started.exchange(true)) {
        WSADATA wsa_data{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            if (error_message != nullptr) {
                *error_message = "WSAStartup failed";
            }
            return false;
        }
    }

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        if (error_message != nullptr) {
            *error_message = "socket() failed: " + std::to_string(WSAGetLastError());
        }
        return false;
    }
    BOOL reuse = TRUE;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<u_short>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        if (error_message != nullptr) {
            *error_message = "bind() failed on port " + std::to_string(port) + ": " +
                             std::to_string(WSAGetLastError());
        }
        ::closesocket(listener);
        return false;
    }
    if (::listen(listener, SOMAXCONN) == SOCKET_ERROR) {
        if (error_message != nullptr) {
            *error_message = "listen() failed: " + std::to_string(WSAGetLastError());
        }
        ::closesocket(listener);
        return false;
    }

    g_listener = listener;
    g_port = port;
    g_running.store(true);
    LogLine("Stellar Lord server listening on 0.0.0.0:" + std::to_string(port));
    LogLine("protocol version " + std::to_string(kProtocolVersion));
    LogLine("Rooms are kept in memory only; nothing is written to disk.");

    std::thread(RoomTickLoop).detach();
    std::thread([]() {
        while (g_running.load()) {
            sockaddr_in client_address{};
            int client_size = sizeof(client_address);
            SOCKET client = ::accept(g_listener,
                                     reinterpret_cast<sockaddr*>(&client_address), &client_size);
            if (client == INVALID_SOCKET) {
                if (!g_running.load()) {
                    break;
                }
                continue;
            }
            if (g_active_clients.load() >= kMaxConcurrentClients) {
                HttpResponse response = ErrorResponse(503, "busy");
                SendAll(client, SerializeResponse(response));
                ::closesocket(client);
                continue;
            }
            g_active_clients.fetch_add(1);
            std::thread([client]() {
                ServeClient(client);
                ::shutdown(client, SD_SEND);
                ::closesocket(client);
                g_active_clients.fetch_sub(1);
            }).detach();
        }
    }).detach();
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) {
        return;
    }
    if (g_listener != INVALID_SOCKET) {
        ::closesocket(g_listener);
        g_listener = INVALID_SOCKET;
    }
    g_port = 0;
    LogLine("shutting down");
}

bool IsRunning() {
    return g_running.load();
}

unsigned short Port() {
    return g_port;
}

}  // namespace rift_server
