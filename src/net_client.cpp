#include "net_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace minimal_taskbar_monitor {

namespace {

bool EnsureWinsock() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, []() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ok;
}

std::string ToLower(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string Trim(std::string text) {
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
    return text.substr(begin, end - begin);
}

bool SendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = ::send(socket, data.data() + sent,
                                 static_cast<int>(std::min<size_t>(data.size() - sent, 32768)), 0);
        if (chunk <= 0) {
            return false;
        }
        sent += static_cast<size_t>(chunk);
    }
    return true;
}

bool ConnectWithTimeout(SOCKET socket, const sockaddr* address, int address_length,
                        int timeout_ms) {
    u_long non_blocking = 1;
    ioctlsocket(socket, FIONBIO, &non_blocking);

    const int rc = ::connect(socket, address, address_length);
    if (rc == 0) {
        u_long blocking = 0;
        ioctlsocket(socket, FIONBIO, &blocking);
        return true;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return false;
    }

    fd_set write_set;
    fd_set error_set;
    FD_ZERO(&write_set);
    FD_ZERO(&error_set);
    FD_SET(socket, &write_set);
    FD_SET(socket, &error_set);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    const int ready = select(0, nullptr, &write_set, &error_set, &timeout);
    if (ready <= 0 || !FD_ISSET(socket, &write_set)) {
        return false;
    }
    int socket_error = 0;
    int length = sizeof(socket_error);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error),
                   &length) != 0 ||
        socket_error != 0) {
        return false;
    }

    u_long blocking = 0;
    ioctlsocket(socket, FIONBIO, &blocking);
    return true;
}

NetResponse PerformRequest(const std::string& host, unsigned short port, bool post,
                           const std::string& path, const std::string& body, int timeout_ms) {
    NetResponse response;
    if (!EnsureWinsock()) {
        response.error = "winsock";
        return response;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    const std::string port_text = std::to_string(port);

    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &addresses) != 0) {
        response.error = "dns";
        return response;
    }

    SOCKET socket = INVALID_SOCKET;
    for (addrinfo* entry = addresses; entry != nullptr; entry = entry->ai_next) {
        SOCKET candidate = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (candidate == INVALID_SOCKET) {
            continue;
        }
        if (ConnectWithTimeout(candidate, entry->ai_addr, static_cast<int>(entry->ai_addrlen),
                               timeout_ms)) {
            socket = candidate;
            break;
        }
        ::closesocket(candidate);
    }
    freeaddrinfo(addresses);

    if (socket == INVALID_SOCKET) {
        response.error = "connect";
        return response;
    }

    DWORD timeout = static_cast<DWORD>(timeout_ms);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout),
               sizeof(timeout));

    std::string request = post ? "POST " : "GET ";
    request += path;
    request += " HTTP/1.1\r\nHost: ";
    request += host + ":" + port_text;
    request += "\r\nUser-Agent: StellarLord/1.0\r\nAccept: */*\r\n";
    if (post) {
        request += "Content-Type: application/x-www-form-urlencoded\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "Connection: close\r\n\r\n";
    if (post) {
        request += body;
    }

    if (!SendAll(socket, request)) {
        ::closesocket(socket);
        response.error = "send";
        return response;
    }

    std::string raw;
    char chunk[8192];
    for (;;) {
        const int received = ::recv(socket, chunk, sizeof(chunk), 0);
        if (received > 0) {
            raw.append(chunk, static_cast<size_t>(received));
            if (raw.size() > 4u * 1024u * 1024u) {
                break;
            }
            continue;
        }
        break;
    }
    ::closesocket(socket);

    const size_t header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        response.error = "recv";
        return response;
    }

    // 状态行：HTTP/1.1 200 OK
    const size_t first_line_end = raw.find("\r\n");
    const std::string status_line = raw.substr(0, first_line_end);
    const size_t first_space = status_line.find(' ');
    if (first_space != std::string::npos) {
        response.status = std::atoi(status_line.c_str() + first_space + 1);
    }

    size_t content_length = 0;
    bool has_content_length = false;
    size_t pos = first_line_end + 2;
    while (pos < header_end) {
        size_t line_end = raw.find("\r\n", pos);
        if (line_end == std::string::npos || line_end > header_end) {
            line_end = header_end;
        }
        const std::string line = raw.substr(pos, line_end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            const std::string key = ToLower(Trim(line.substr(0, colon)));
            const std::string value = Trim(line.substr(colon + 1));
            if (key == "content-length") {
                content_length = static_cast<size_t>(std::atoll(value.c_str()));
                has_content_length = true;
            }
        }
        if (line_end == header_end) {
            break;
        }
        pos = line_end + 2;
    }

    const size_t body_begin = header_end + 4;
    if (has_content_length && raw.size() >= body_begin) {
        response.body = raw.substr(body_begin, std::min(content_length, raw.size() - body_begin));
    } else {
        response.body = raw.substr(body_begin);
    }
    response.transport_ok = true;
    return response;
}

}  // namespace

NetResponse HttpGet(const std::string& host, unsigned short port, const std::string& path,
                    int timeout_ms) {
    return PerformRequest(host, port, false, path, {}, timeout_ms);
}

NetResponse HttpPostForm(const std::string& host, unsigned short port, const std::string& path,
                         const std::string& form_body, int timeout_ms) {
    return PerformRequest(host, port, true, path, form_body, timeout_ms);
}

std::string UrlEncode(const std::string& text) {
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

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

}  // namespace minimal_taskbar_monitor
