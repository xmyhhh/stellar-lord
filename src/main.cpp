#include "app_config.h"
#include "background_worker.h"
#include "game_model.h"
#include "net_client.h"
#include "process_monitor.h"
#include "resource.h"
#include "server_core.h"
#include "system_metrics.h"
#include "taskbar_embedder.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>
#include <iptypes.h>
#include <shellapi.h>
#include <windowsx.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace minimal_taskbar_monitor {

namespace {

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

constexpr wchar_t kControllerClassName[] = L"StellarLordControllerWindow";
constexpr wchar_t kWidgetClassName[] = L"StellarLordWidgetWindow";
constexpr wchar_t kPopupClassName[] = L"StellarLordPopupWindow";
constexpr wchar_t kServerClassName[] = L"StellarLordServerWindow";
constexpr wchar_t kRoomClassName[] = L"StellarLordRoomWindow";
constexpr wchar_t kLaunchClassName[] = L"StellarLordLaunchWindow";
constexpr wchar_t kHostClassName[] = L"StellarLordHostWindow";

constexpr UINT_PTR kSampleTimerId = 1;
constexpr UINT_PTR kLayoutTimerId = 2;
constexpr UINT_PTR kReattachTimerId = 3;
constexpr UINT_PTR kHoverHideTimerId = 4;
constexpr UINT_PTR kGameTickTimerId = 5;
constexpr UINT_PTR kRoomPollTimerId = 6;

constexpr int kToggleModeHotkeyId = 1;
constexpr UINT kLayoutIntervalMs = 1000;
constexpr UINT kReattachDelayMs = 600;
constexpr UINT kHoverHideDelayMs = 180;
constexpr UINT kGameTickIntervalMs = 1000;
constexpr UINT kRoomPollIntervalMs = 1000;

constexpr UINT kTrayIconCallbackMessage = WM_APP + 1;
constexpr UINT kSystemSampleReadyMessage = WM_APP + 2;
constexpr UINT kWidgetRedrawMessage = WM_APP + 3;
constexpr UINT kNetResultMessage = WM_APP + 4;
constexpr UINT kTrayIconId = 1;

constexpr UINT kExitCommandId = 1001;
constexpr UINT kAutoStartCommandId = 1002;
constexpr UINT kModeMonitorCommandId = 1003;
constexpr UINT kModeLordCommandId = 1004;
constexpr UINT kPopupHoverCommandId = 1005;
constexpr UINT kPopupClickCommandId = 1006;
constexpr UINT kInterval1sCommandId = 1007;
constexpr UINT kInterval2sCommandId = 1008;
constexpr UINT kInterval5sCommandId = 1009;
constexpr UINT kInterval10sCommandId = 1010;
constexpr UINT kHotkeyAltQCommandId = 1011;
constexpr UINT kHotkeyAltSCommandId = 1012;
constexpr UINT kHotkeyAltMCommandId = 1013;
constexpr UINT kHotkeyCtrlAltQCommandId = 1014;
constexpr UINT kLanguageEnglishCommandId = 1015;
constexpr UINT kLanguageChineseCommandId = 1016;
constexpr UINT kHelpCommandId = 1017;
constexpr UINT kResetSaveCommandId = 1018;
constexpr UINT kOnlineCommandId = 1019;
constexpr UINT kHostCommandId = 1020;
constexpr UINT kInviteCommandId = 1021;

constexpr UINT kServerSaveCommandId = 2101;
constexpr UINT kServerCancelCommandId = 2102;

constexpr UINT kRoomJoinCommandId = 2201;
constexpr UINT kRoomCancelCommandId = 2202;
constexpr UINT kRoomPasteCommandId = 2203;

constexpr UINT kLaunchServerCommandId = 2301;
constexpr UINT kLaunchClientCommandId = 2302;

constexpr UINT kHostCopyCommandId = 2401;
constexpr UINT kHostEnterCommandId = 2402;

constexpr int kServerWindowWidth = 380;
constexpr int kServerWindowHeight = 210;
constexpr int kRoomWindowWidth = 520;
constexpr int kRoomWindowHeight = 250;
constexpr int kLaunchWindowWidth = 480;
constexpr int kLaunchWindowHeight = 245;
constexpr int kHostWindowWidth = 560;
constexpr int kHostWindowHeight = 250;

constexpr wchar_t kRunRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"StellarLordTaskbar";

// ---------------------------------------------------------------------------
// 语言
// ---------------------------------------------------------------------------

UiLanguage g_language = UiLanguage::kChinese;

const wchar_t* T(const wchar_t* english, const wchar_t* chinese) {
    return g_language == UiLanguage::kEnglish ? english : chinese;
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

int ScaleByDpi(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool IsSupportedSampleIntervalSeconds(unsigned int seconds) {
    return seconds == 1 || seconds == 2 || seconds == 5 || seconds == 10;
}

UINT GetSampleTimerIntervalMs(unsigned int seconds) {
    if (!IsSupportedSampleIntervalSeconds(seconds)) {
        seconds = 1;
    }
    return seconds * 1000u;
}

std::wstring FormatW(const wchar_t* format, ...) {
    wchar_t buffer[512]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    return std::wstring(buffer);
}

std::wstring FormatCompact(double value) {
    if (value < 0.0) {
        return L"-" + FormatCompact(-value);
    }
    if (value < 1000.0) {
        return FormatW(L"%.0f", value);
    }
    const wchar_t* units[] = {L"", L"k", L"M", L"B", L"T", L"P"};
    int unit = 0;
    double scaled = value;
    while (scaled >= 1000.0 && unit < 5) {
        scaled /= 1000.0;
        ++unit;
    }
    if (scaled >= 100.0) {
        return FormatW(L"%.0f%s", scaled, units[unit]);
    }
    if (scaled >= 10.0) {
        return FormatW(L"%.1f%s", scaled, units[unit]);
    }
    return FormatW(L"%.2f%s", scaled, units[unit]);
}

std::wstring FormatDuration(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const long long total = static_cast<long long>(seconds);
    if (total < 60) {
        return FormatW(L"%llds", total);
    }
    const long long minutes = total / 60;
    if (minutes < 60) {
        return FormatW(L"%lldm%02llds", minutes, total % 60);
    }
    const long long hours = minutes / 60;
    if (hours < 24) {
        return FormatW(L"%lldh%02lldm", hours, minutes % 60);
    }
    return FormatW(L"%lldd%02lldh", hours / 24, hours % 24);
}

std::wstring FormatBytes(unsigned long long bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        return FormatW(L"%.0f%s", value, units[unit]);
    }
    return FormatW(L"%.1f%s", value, units[unit]);
}

std::wstring FormatPercent(double percent) {
    if (percent < 0.0) {
        return L"--";
    }
    return FormatW(L"%.0f%%", percent);
}

std::wstring FormatSignedPercent(double percent) {
    return FormatW(L"%+.1f%%", percent);
}

HFONT CreatePreferredUiFont(UINT dpi, int point_size) {
    LOGFONTW font_template{};
    font_template.lfHeight = -ScaleByDpi(dpi, point_size);
    font_template.lfWeight = FW_NORMAL;
    font_template.lfCharSet = DEFAULT_CHARSET;
    font_template.lfOutPrecision = OUT_DEFAULT_PRECIS;
    font_template.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    font_template.lfQuality = CLEARTYPE_NATURAL_QUALITY;
    font_template.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(font_template.lfFaceName, L"Microsoft YaHei UI");

    return CreateFontIndirectW(&font_template);
}

struct WidgetPalette {
    COLORREF background;
    COLORREF border;
    COLORREF primary_text;
    COLORREF secondary_text;
};

WidgetPalette GetWidgetPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(244, 245, 247), RGB(214, 217, 221), RGB(61, 65, 71), RGB(95, 101, 109)};
    }
    return {RGB(36, 39, 44), RGB(65, 70, 77), RGB(204, 209, 216), RGB(170, 177, 187)};
}

struct PopupPalette {
    COLORREF background;
    COLORREF border;
    COLORREF title_text;
    COLORREF primary_text;
    COLORREF secondary_text;
    COLORREF accent;
    COLORREF header_fill;
    COLORREF row_highlight;
    COLORREF tab_active_fill;
    COLORREF disabled_text;
    COLORREF good_text;
    COLORREF warn_text;
    COLORREF bad_text;
};

PopupPalette GetPopupPalette(bool light_theme) {
    if (light_theme) {
        return {RGB(248, 249, 250), RGB(215, 219, 224), RGB(33, 37, 43),
                RGB(45, 51, 59),    RGB(105, 113, 123), RGB(37, 99, 168),
                RGB(235, 238, 242), RGB(226, 232, 240), RGB(214, 224, 238),
                RGB(158, 166, 176), RGB(22, 122, 74),   RGB(150, 96, 12),
                RGB(168, 45, 45)};
    }
    return {RGB(28, 31, 36),    RGB(58, 64, 72),    RGB(226, 231, 237),
            RGB(208, 214, 222), RGB(140, 150, 163), RGB(96, 165, 250),
            RGB(38, 43, 50),    RGB(46, 52, 60),    RGB(45, 55, 70),
            RGB(96, 104, 114),  RGB(74, 222, 128),  RGB(250, 204, 21),
            RGB(248, 113, 113)};
}

SIZE MeasureTextSize(HDC dc, const std::wstring& text) {
    SIZE size{};
    if (!text.empty()) {
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    }
    return size;
}

std::vector<int> MeasureColumnWidths(HDC dc, const std::vector<DisplayLines::Column>& columns) {
    std::vector<int> column_widths;
    column_widths.reserve(columns.size());
    for (const auto& column : columns) {
        const SIZE top_size = MeasureTextSize(dc, column.top_text);
        const SIZE bottom_size = MeasureTextSize(dc, column.bottom_text);
        column_widths.push_back(std::max<int>(static_cast<int>(top_size.cx),
                                              static_cast<int>(bottom_size.cx)));
    }
    return column_widths;
}

int SumColumnWidths(const std::vector<int>& column_widths, int column_gap) {
    int total = 0;
    for (std::size_t i = 0; i < column_widths.size(); ++i) {
        if (i != 0) {
            total += column_gap;
        }
        total += column_widths[i];
    }
    return total;
}

bool IsPointInWindowRect(HWND window_handle, const POINT& screen_point) {
    if (window_handle == nullptr || !IsWindow(window_handle)) {
        return false;
    }
    RECT rect{};
    if (!GetWindowRect(window_handle, &rect)) {
        return false;
    }
    return PtInRect(&rect, screen_point) != FALSE;
}

std::wstring GetClockText() {
    SYSTEMTIME local_time{};
    GetLocalTime(&local_time);
    return FormatW(L"%02u:%02u", local_time.wHour, local_time.wMinute);
}

std::wstring TrimWide(std::wstring text) {
    size_t begin = 0;
    size_t end = text.size();
    auto is_space = [](wchar_t ch) {
        return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
    };
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::wstring FormatUnixClock(double unix_time) {
    if (unix_time <= 0.0) {
        return L"--:--";
    }
    const std::time_t value = static_cast<std::time_t>(unix_time);
    std::tm local_time{};
    if (localtime_s(&local_time, &value) != 0) {
        return L"--:--";
    }
    return FormatW(L"%02d/%02d %02d:%02d", local_time.tm_mon + 1, local_time.tm_mday,
                   local_time.tm_hour, local_time.tm_min);
}

// 服务端响应是 key=value 行文本，取第一行的值就够了。
std::string NetField(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equal = line.find('=');
        if (equal != std::string::npos && line.compare(0, equal, key) == 0 &&
            line.substr(0, equal) == key) {
            return line.substr(equal + 1);
        }
        pos = end + 1;
    }
    return {};
}

// 同一个 key 可能出现多次（例如 member=）。
std::vector<std::string> NetFields(const std::string& body, const std::string& key) {
    std::vector<std::string> values;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find('\n', pos);
        if (end == std::string::npos) {
            end = body.size();
        }
        std::string line = body.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t equal = line.find('=');
        if (equal != std::string::npos && line.substr(0, equal) == key) {
            values.push_back(line.substr(equal + 1));
        }
        pos = end + 1;
    }
    return values;
}

std::vector<std::string> SplitPipe(const std::string& value) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t bar = value.find('|', start);
        if (bar == std::string::npos) {
            parts.push_back(value.substr(start));
            break;
        }
        parts.push_back(value.substr(start, bar - start));
        start = bar + 1;
    }
    return parts;
}

// ---------------------------------------------------------------------------
// 邀请链接与剪贴板
// ---------------------------------------------------------------------------

struct InviteInfo {
    std::string host;
    unsigned int port{0};
    std::string room;
};

std::string TrimAscii(std::string text) {
    size_t begin = 0;
    size_t end = text.size();
    auto space = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '"' || ch == '\'';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string PercentDecodeAscii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            const int high = hex(text[i + 1]);
            const int low = hex(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

std::string QueryParam(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t next = query.find('&', pos);
        if (next == std::string::npos) {
            next = query.size();
        }
        const std::string pair = query.substr(pos, next - pos);
        const size_t equal = pair.find('=');
        if (equal != std::string::npos) {
            const std::string name = ToLowerAscii(TrimAscii(pair.substr(0, equal)));
            if (name == ToLowerAscii(key)) {
                return PercentDecodeAscii(TrimAscii(pair.substr(equal + 1)));
            }
        }
        if (next == query.size()) {
            break;
        }
        pos = next + 1;
    }
    return {};
}

std::string NormalizeRoomCode(std::string code) {
    code = TrimAscii(code);
    std::string out;
    for (char ch : code) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    return out;
}

// 支持三种形式：
//   stellar://join?host=192.168.1.5&port=8787&room=ABCD
//   http://192.168.1.5:8787/join/ABCD
//   ABCD（沿用当前连接地址）
bool ParseInviteText(const std::wstring& text, InviteInfo& out, std::wstring* error) {
    std::string raw = TrimAscii(WideToUtf8(text));
    if (raw.empty()) {
        if (error != nullptr) *error = T(L"Paste an invite link first.", L"请先粘贴邀请链接。");
        return false;
    }

    std::string work = raw;
    const size_t scheme = work.find("://");
    if (scheme != std::string::npos) {
        work = work.substr(scheme + 3);
    }

    std::string authority;
    std::string query;
    std::string path;
    const size_t question = work.find('?');
    std::string before_query = work;
    if (question != std::string::npos) {
        before_query = work.substr(0, question);
        std::string rest = work.substr(question + 1);
        const size_t hash = rest.find('#');
        if (hash != std::string::npos) {
            rest = rest.substr(0, hash);
        }
        query = rest;
    }
    const size_t slash = before_query.find('/');
    if (slash != std::string::npos) {
        authority = before_query.substr(0, slash);
        path = before_query.substr(slash);
    } else {
        authority = before_query;
    }

    std::string host = QueryParam(query, "host");
    std::string port_text = QueryParam(query, "port");
    std::string room = QueryParam(query, "room");

    // URL 形式没有 host=/room= 参数，从 authority 和 path 里拆。
    const bool looks_like_plain_code =
        scheme == std::string::npos && query.empty() && path.empty();
    if (host.empty() && !looks_like_plain_code) {
        const std::string lower_authority = ToLowerAscii(authority);
        if (!authority.empty() && lower_authority != "join" && lower_authority != "r" &&
            lower_authority != "room") {
            const size_t colon = authority.rfind(':');
            if (colon != std::string::npos && authority.find(']') == std::string::npos) {
                host = authority.substr(0, colon);
                if (port_text.empty()) {
                    port_text = authority.substr(colon + 1);
                }
            } else {
                host = authority;
            }
        }
    }
    if (room.empty()) {
        std::string lower_path = ToLowerAscii(path);
        for (const char* prefix : {"/join/", "/r/", "/room/"}) {
            const size_t pos = lower_path.find(prefix);
            if (pos != std::string::npos) {
                room = path.substr(pos + std::strlen(prefix));
                const size_t end = room.find_first_of("/?#");
                if (end != std::string::npos) {
                    room = room.substr(0, end);
                }
                break;
            }
        }
    }
    if (room.empty() && looks_like_plain_code) {
        room = raw;
    }

    room = NormalizeRoomCode(room);
    if (room.size() < 4 || room.size() > 12) {
        if (error != nullptr) {
            *error = T(L"Invite link has no valid rift code (4-12 letters/digits).",
                       L"链接里没有有效的时空码（4-12 位字母或数字）。");
        }
        return false;
    }

    host = TrimAscii(host);
    unsigned int port = 0;
    if (!port_text.empty()) {
        const int value = std::atoi(port_text.c_str());
        if (value <= 0 || value > 65535) {
            if (error != nullptr) *error = T(L"Invalid port.", L"链接里的端口无效。");
            return false;
        }
        port = static_cast<unsigned int>(value);
    }
    if (!host.empty()) {
        const size_t colon = host.rfind(':');
        if (colon != std::string::npos && host.find(']') == std::string::npos) {
            const int value = std::atoi(host.c_str() + colon + 1);
            if (value > 0 && value <= 65535) {
                port = static_cast<unsigned int>(value);
                host = host.substr(0, colon);
            }
        }
        if (host.empty() || host.size() > 253) {
            if (error != nullptr) *error = T(L"Invalid host in invite link.", L"链接里的主机地址无效。");
            return false;
        }
    }

    out.host = host;
    out.port = port;
    out.room = room;
    return true;
}

std::wstring BuildInviteLink(const std::string& host, unsigned int port,
                             const std::string& room) {
    std::string link = "stellar://join?host=" + UrlEncode(host.empty() ? "127.0.0.1" : host) +
                       "&port=" + std::to_string(port == 0 ? 8787u : port) +
                       "&room=" + UrlEncode(NormalizeRoomCode(room));
    return Utf8ToWide(link);
}

bool WriteClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool copied = false;
    if (memory != nullptr) {
        void* target = GlobalLock(memory);
        if (target != nullptr) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                copied = true;
            }
        }
        if (!copied) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return copied;
}

bool ReadClipboardText(HWND owner, std::wstring& text) {
    text.clear();
    if (!OpenClipboard(owner)) {
        return false;
    }
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    bool ok = false;
    if (handle != nullptr) {
        const auto* source = static_cast<const wchar_t*>(GlobalLock(handle));
        if (source != nullptr) {
            text = source;
            GlobalUnlock(handle);
            ok = true;
        }
    }
    CloseClipboard();
    return ok;
}

std::string DetectLanIpv4() {
    ULONG size = 15 * 1024;
    std::vector<unsigned char> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, addresses, &size);
    }
    if (result != NO_ERROR) {
        return "127.0.0.1";
    }

    std::string best;
    int best_score = -1;
    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr ||
                unicast->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* address =
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
            char text[64]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) == nullptr) {
                continue;
            }
            const std::string ip = text;
            if (ip.rfind("127.", 0) == 0 || ip == "0.0.0.0") {
                continue;
            }
            int score = 1;
            if (ip.rfind("192.168.", 0) == 0 || ip.rfind("10.", 0) == 0) {
                score = 3;
            } else if (ip.rfind("172.", 0) == 0) {
                const int second = std::atoi(ip.c_str() + 4);
                if (second >= 16 && second <= 31) {
                    score = 2;
                }
            } else if (ip.rfind("169.254.", 0) == 0) {
                score = 0;
            }
            if (score > best_score) {
                best_score = score;
                best = ip;
            }
        }
    }
    return best.empty() ? "127.0.0.1" : best;
}

// ---------------------------------------------------------------------------
// 领主面板数据
// ---------------------------------------------------------------------------

enum class FreightTab {
    kDevelopment = 0,
    kResearch = 1,
    kMilitary = 2,
    kEmpire = 3,
    kTrade = 4,
    kExpedition = 5,
    kEvents = 6,
    kOnline = 7,
    kCount = 8,
};

const wchar_t* FreightTabLabel(int index) {
    switch (index) {
    case 0: return T(L"Empire", L"发展");
    case 1: return T(L"Science", L"科研");
    case 2: return T(L"Fleet", L"军事");
    case 3: return T(L"Stars", L"星图");
    case 4: return T(L"Trade", L"贸易");
    case 5: return T(L"Expedition", L"远征");
    case 6: return T(L"Events", L"事件");
    case 7:
    default: return T(L"Rift", L"联机");
    }
}

enum class RowAction {
    kNone,
    kBuild,
    kCapital,
    kResearchTech,
    kBuildFleet,
    kAttackNeutral,
    kAttackSector,
    kColonize,
    kColonyUpgrade,
    kAttackRival,
    kBuildWonder,
    kChooseFaction,
    kResolveEvent,
    kTradeResource,
    kFulfillContract,
    kBuyBlackMarket,
    kLaunchExpedition,
    kClearExpeditionReports,
    kChooseCommandCard,
    kAuditRelief,
    kCopyPlayerId,
    kCopyInviteLink,
    kEditServer,
    kRoomCreate,
    kRoomJoinPrompt,
    kRoomJoinCode,
    kRoomLeave,
    kRoomStart,
    kRoomRefresh,
    kStartHosting,
};

struct FreightRow {
    std::wstring left;
    std::wstring mid;
    std::wstring right;
    RowAction action{RowAction::kNone};
    int index{-1};
    bool enabled{true};
    bool has_choice{false};
    std::wstring choice_a;
    std::wstring choice_b;
    bool choice_a_enabled{true};
    bool choice_b_enabled{true};
};

struct FreightLayoutRects {
    std::vector<RECT> tabs;
    std::vector<RECT> rows;
    std::vector<RECT> rows_a;
    std::vector<RECT> rows_b;
    RECT footer{};
};

struct SystemSampleResult {
    MetricsSnapshot metrics;
    std::optional<ProcessPopupSnapshot> processes;
};

// -- 联机 -------------------------------------------------------------------

enum class NetTaskKind {
    kRoomCreate,
    kRoomJoin,
    kRoomLeave,
    kRoomStart,
    kRoomAction,
    kRoomState,
    kRoomList,
};

struct NetTaskResult {
    NetTaskKind kind{NetTaskKind::kRoomState};
    NetResponse response;
    std::string tag;
};

struct RoomMemberInfo {
    std::string id;
    std::string name;
    double score{0.0};
    int fleet{0};
    int colonies{0};
    int capital{1};
    bool online{true};
    bool me{false};
    double tokens{0.0};
    double influence{0.0};
    int relics{0};
    int cores{0};
    int data_cores{0};
    int reputation{50};
};

struct RoomListEntry {
    std::string code;
    std::string host_name;
    int players{0};
    bool started{false};
};

struct EventDisplay {
    const wchar_t* title;
    const wchar_t* description;
    const wchar_t* choice_a;
    const wchar_t* choice_b;
};

EventDisplay GetEventDisplay(int id) {
    switch (static_cast<stellar::EventId>(id)) {
    case stellar::EventId::kPirateRaid:
        return {T(L"Pirate raid", L"海盗劫掠"),
                T(L"Raiders are circling your supply line.",
                  L"一伙海盗盯上了你的补给线。"),
                T(L"Escort -40F -20A", L"组织护航  -40粮 -20合"),
                T(L"Pay -50M", L"交保护费  -50矿")};
    case stellar::EventId::kIonStorm:
        return {T(L"Ion storm", L"离子风暴"),
                T(L"The storm can be mined for data, or avoided.",
                  L"风暴里能捞到数据，也可能烧掉电网。"),
                T(L"Harvest -60E +SCI", L"冒险采集  -60电 +科研"),
                T(L"Shield -20E", L"关闭护盾  -20电")};
    case stellar::EventId::kRefugeeFleet:
        return {T(L"Refugee fleet", L"难民船队"),
                T(L"A damaged convoy asks for shelter.",
                  L"一支受损船队请求庇护。"),
                T(L"Accept -80F +POP", L"接收难民  -80粮 +人口"),
                T(L"Aid -30F +SCI", L"补给后送  -30粮 +科研")};
    case stellar::EventId::kBlackMarket:
        return {T(L"Black market", L"黑市报价"),
                T(L"Hecate sells a data core with no receipt.",
                  L"赫卡忒在卖一枚没有发票的数据核心。"),
                T(L"Buy -120M +SCI", L"买下科技  -120矿 +科研"),
                T(L"Report +45A", L"举报黑市  +45合金")};
    case stellar::EventId::kMineCollapse:
        return {T(L"Mine collapse", L"矿脉塌方"),
                T(L"Miners are trapped below the main shaft.",
                  L"主矿脉塌方，矿工被困在下面。"),
                T(L"Rescue -70M -40E", L"全力救援  -70矿 -40电"),
                T(L"Seal -30M", L"封矿止损  -30矿")};
    case stellar::EventId::kSilenceScan:
        return {T(L"Silence scan", L"静默者扫描"),
                T(L"An audit probe is sweeping the sector.",
                  L"一枚审计探针正在扫描星区。"),
                T(L"Go dark -50E", L"静默潜行  -50电"),
                T(L"Scan back +SCI", L"反向扫描  +科研 -舰队")};
    case stellar::EventId::kDeserters:
        return {T(L"Deserters", L"逃兵舰队"),
                T(L"A fleet offers service for food.",
                  L"一支逃兵舰队说，只要一口饭就替你打仗。"),
                T(L"Recruit -60F +FLT", L"收编  -60粮 +舰队"),
                T(L"Disarm +60A", L"缴械  +60合金")};
    case stellar::EventId::kTradeBoom:
        return {T(L"Trade boom", L"贸易旺季"),
                T(L"Prices spike before the rift closes.",
                  L"裂谷关闭前，市场价格突然走高。"),
                T(L"Sell -60M +110A", L"全速出货  -60矿 +110合金"),
                T(L"Wait +SCI", L"囤货观望  +科研")};
    case stellar::EventId::kEchoSignal:
        return {T(L"Echo signal", L"回声信号"),
                T(L"A fragment repeats on a dead frequency. It knows your name.",
                  L"一段碎片在死频上重复。它知道你的名字。"),
                T(L"Tune -60P +DATA", L"调谐阵列  -60电 +数据核心"),
                T(L"Sell +50ST", L"卖给赫卡忒  +50 ST")};
    case stellar::EventId::kArkDistress:
        return {T(L"Ark distress", L"方舟求救"),
                T(L"A generation ship is venting atmosphere. Someone is still aboard.",
                  L"一艘世代船正在泄压。上面还有人活着。"),
                T(L"Rescue -100F -20A +POP", L"救援  -100粮 -20合 +人口"),
                T(L"Salvage +80A +30ST", L"打捞  +80合 +30 ST -士气")};
    case stellar::EventId::kSilentTide:
        return {T(L"Silent tide", L"静默潮汐"),
                T(L"The audit wave passes close. Sensors scream in flat tones.",
                  L"审计波贴着星区扫过。传感器用平直的音调尖叫。"),
                T(L"Evade -80P +SCI", L"全舰规避  -80电 +科研"),
                T(L"Sample -6FLT +COR", L"强行采样  -6舰 +星核")};
    case stellar::EventId::kCount:
    default:
        return {T(L"Unknown event", L"未知事件"), L"", L"", L""};
    }
}

const wchar_t* FactionName(int faction) {
    switch (static_cast<stellar::FactionId>(faction)) {
    case stellar::FactionId::kAnvil: return T(L"Anvil Industry", L"铁砧工业");
    case stellar::FactionId::kChoir: return T(L"Star Choir", L"星海教团");
    case stellar::FactionId::kCompact: return T(L"Frontier Compact", L"拓荒议会");
    case stellar::FactionId::kGuild: return T(L"Free Guild", L"自由商盟");
    case stellar::FactionId::kNone:
    case stellar::FactionId::kCount:
    default: return T(L"Unaligned", L"未选择");
    }
}

const wchar_t* FactionDescription(int faction) {
    switch (static_cast<stellar::FactionId>(faction)) {
    case stellar::FactionId::kAnvil:
        return T(L"Alloy +30%, but power is always hungry", L"合金 +30%，但电网常年喊饿");
    case stellar::FactionId::kChoir:
        return T(L"Research +35%, power output -4%", L"科研 +35%，能源产出 -4%");
    case stellar::FactionId::kCompact:
        return T(L"Pop cap +25%, growth +15%, +8 morale",
                 L"人口上限 +25%，增长 +15%，士气 +8");
    case stellar::FactionId::kGuild:
        return T(L"Loot +25%, ships -15%, alloy -5%",
                 L"掠夺 +25%，造船 -15%，合金 -5%");
    case stellar::FactionId::kNone:
    case stellar::FactionId::kCount:
    default:
        return L"";
    }
}

const wchar_t* WonderName(int wonder) {
    switch (wonder) {
    case stellar::kWonderDysonRing: return T(L"Dyson Ring", L"戴森环");
    case stellar::kWonderKnowledgeArk: return T(L"Knowledge Ark", L"知识方舟");
    case stellar::kWonderOrbitalDock: return T(L"Orbital Dock", L"轨道船坞");
    default: return L"";
    }
}

const wchar_t* WonderDescription(int wonder) {
    switch (wonder) {
    case stellar::kWonderDysonRing:
        return T(L"Power output +100%", L"能源产出 +100%");
    case stellar::kWonderKnowledgeArk:
        return T(L"Research +100%, tech cost -25% (needs 300 SCI)",
                 L"科研 +100%，科技成本 -25%（需要 300 科研）");
    case stellar::kWonderOrbitalDock:
        return T(L"Fleet cap +50%, training +50%", L"舰队上限 +50%，训练 +50%");
    default:
        return L"";
    }
}

const wchar_t* ResourceName(int resource) {
    switch (resource) {
    case 0: return T(L"Energy", L"能源");
    case 1: return T(L"Minerals", L"矿物");
    case 2: return T(L"Food", L"食物");
    case 3: return T(L"Alloy", L"合金");
    default: return L"";
    }
}

const wchar_t* EraName(int era) {
    switch (era) {
    case 0: return T(L"Awakening", L"苏醒");
    case 1: return T(L"First Era: Footing", L"第一时代：立足");
    case 2: return T(L"Second Era: Expansion", L"第二时代：扩张");
    case 3: return T(L"Third Era: Audit", L"第三时代：审计");
    case 4:
    default: return T(L"Reckoning", L"结算");
    }
}

const wchar_t* EraFlavor(int era) {
    switch (era) {
    case 0:
        return T(L"ARK-3: Welcome to your capital. Strictly speaking, it is a coordinate with a roof.",
                 L"阿刻：欢迎来到你的首都。严格来说，它只是一座有屋顶的坐标。");
    case 1:
        return T(L"Elias Wen: In principle this is illegal, but it is tide season.",
                 L"伊莱亚斯·文：原则上这是违法的，但现在是潮汐期。");
    case 2:
        return T(L"Hecate: Prices are alive. So are your neighbours.",
                 L"赫卡忒：价格是活的。你的邻居也是。");
    case 3:
        return T(L"The Silence: Deviation detected. Beginning archival.",
                 L"静默者：检测到偏差。开始归档。");
    case 4:
    default:
        return T(L"The rift is closing. Take what you can carry.",
                 L"裂谷正在关闭。把该带走的都带走。");
    }
}

const wchar_t* AuditTierName(int tier) {
    switch (tier) {
    case 0: return T(L"Quiet", L"静默");
    case 1: return T(L"Sampled", L"采样中");
    case 2: return T(L"Tide", L"潮汐中");
    case 3: return T(L"Archiving", L"归档中");
    default: return T(L"Unknown", L"未知");
    }
}

const wchar_t* AuditFlavor(int tier) {
    switch (tier) {
    case 0:
        return T(L"ARK-3: No deviations logged. That is not the same as no deviations.",
                 L"阿刻：没有记录到偏差。这和没有偏差不是一回事。");
    case 1:
        return T(L"The Silence: Sample acquired. Thank you for your cooperation.",
                 L"静默者：样本已获取。感谢配合。");
    case 2:
        return T(L"The Silence: Deviation is no longer local. Tide is rising.",
                 L"静默者：偏差已不再局部。潮汐正在上涨。");
    case 3:
    default:
        return T(L"The Silence: Archiving is not killing. Continue your civilization.",
                 L"静默者：归档不是杀戮。请继续你的文明。");
    }
}

const wchar_t* ExpeditionName(int destination) {
    switch (static_cast<stellar::ExpeditionDestination>(destination)) {
    case stellar::ExpeditionDestination::kFrontier: return T(L"Uncharted sector", L"未探索扇区");
    case stellar::ExpeditionDestination::kRuins: return T(L"Ruins belt", L"遗迹带");
    case stellar::ExpeditionDestination::kAnomaly: return T(L"Deep anomaly", L"深空异常");
    case stellar::ExpeditionDestination::kArk: return T(L"Drifting ark", L"漂流方舟");
    case stellar::ExpeditionDestination::kCount:
    default: return L"";
    }
}

const wchar_t* ExpeditionDescription(int destination) {
    switch (static_cast<stellar::ExpeditionDestination>(destination)) {
    case stellar::ExpeditionDestination::kFrontier:
        return T(L"low risk · charts, colony leads, small salvage",
                 L"低风险 · 地图情报、殖民点、少量打捞");
    case stellar::ExpeditionDestination::kRuins:
        return T(L"medium risk · relics, data cores, echoes",
                 L"中风险 · 遗物、数据核心、回声剧情");
    case stellar::ExpeditionDestination::kAnomaly:
        return T(L"high risk · star cores, rare tech",
                 L"高风险 · 星核、稀有科技");
    case stellar::ExpeditionDestination::kArk:
        return T(L"mid-high risk · data cores, people, old tech",
                 L"中高风险 · 数据核心、人口、旧科技");
    case stellar::ExpeditionDestination::kCount:
    default:
        return L"";
    }
}

const wchar_t* ExpeditionStanceName(int stance) {
    switch (static_cast<stellar::ExpeditionStance>(stance)) {
    case stellar::ExpeditionStance::kCautious: return T(L"Cautious", L"谨慎");
    case stellar::ExpeditionStance::kDeep: return T(L"Deep", L"深入");
    case stellar::ExpeditionStance::kCount:
    default: return L"";
    }
}

const wchar_t* ExpeditionOutcomeName(int outcome) {
    switch (outcome) {
    case 0: return T(L"Great success", L"大成功");
    case 1: return T(L"Success", L"成功");
    case 2: return T(L"Partial success", L"部分成功");
    case 3: return T(L"Failure", L"失败");
    case 4: return T(L"Disaster", L"灾难");
    default: return T(L"Unknown", L"未知");
    }
}

const wchar_t* ExpeditionFlavor(int destination, int outcome) {
    if (outcome >= 3) {
        return T(L"ARK-3: The rift keeps what it takes. We keep the names.",
                 L"阿刻：裂谷收走了该收的，我们只留下名字。");
    }
    switch (static_cast<stellar::ExpeditionDestination>(destination)) {
    case stellar::ExpeditionDestination::kFrontier:
        return T(L"ARK-3: Another empty rock. Another useful rock.",
                 L"阿刻：又一块空石头。又一块有用的石头。");
    case stellar::ExpeditionDestination::kRuins:
        return T(L"ARK-3: The echo does not belong to any fleet on record.",
                 L"阿刻：这段回声不属于任何在册舰队。");
    case stellar::ExpeditionDestination::kAnomaly:
        return T(L"ARK-3: The sensor readings are illegal in three jurisdictions.",
                 L"阿刻：这些传感器读数在三个辖区都算违法。");
    case stellar::ExpeditionDestination::kArk:
        return T(L"ARK-3: The ark is still calling a port that no longer exists.",
                 L"阿刻：方舟还在呼叫一个已经不存在的港口。");
    default:
        return L"";
    }
}

const wchar_t* BlackMarketName(int kind) {
    switch (kind) {
    case 0: return T(L"Relic", L"遗物");
    case 1: return T(L"Star core", L"星核");
    case 2: return T(L"Data core", L"数据核心");
    case 3: return T(L"Mercenaries", L"雇佣兵");
    case 4: return T(L"Research cache", L"科研缓存");
    case 5: return T(L"Political favour", L"政治献金");
    default: return T(L"Unknown lot", L"未知商品");
    }
}

const wchar_t* BlackMarketDescription(int kind, int amount) {
    switch (kind) {
    case 0: return T(L"rare tech / wonder component", L"稀有科技 / 奇观构件");
    case 1: return T(L"flagship / gate component", L"旗舰 / 星门构件");
    case 2: return T(L"tech / star chart / echo clue", L"科技 / 星图 / 回声线索");
    case 3:
        return amount > 0 ? T(L"six ships join your fleet", L"6 艘舰船加入舰队")
                          : L"";
    case 4: return T(L"one-time research boost", L"一次性科研注入");
    case 5: return T(L"influence, at a terrible rate", L"影响力，汇率很差");
    default: return L"";
    }
}

const wchar_t* CommandCardName(int card) {
    switch (static_cast<stellar::CommandCardId>(card)) {
    case stellar::CommandCardId::kEmergencyLevy: return T(L"Emergency levy", L"紧急征召");
    case stellar::CommandCardId::kResearchBreakthrough:
        return T(L"Research breakthrough", L"科研突破");
    case stellar::CommandCardId::kColonySubsidy: return T(L"Colony subsidy", L"殖民补贴");
    case stellar::CommandCardId::kEmergencyLoan: return T(L"Emergency loan", L"紧急贷款");
    case stellar::CommandCardId::kTalentIntake: return T(L"Talent intake", L"人才引进");
    case stellar::CommandCardId::kDiplomaticRound:
        return T(L"Diplomatic round", L"外交斡旋");
    case stellar::CommandCardId::kBlackMarketRun:
        return T(L"Black market run", L"黑市采购");
    case stellar::CommandCardId::kSilenceListening:
        return T(L"Silence listening", L"静默监听");
    case stellar::CommandCardId::kCount:
    default: return T(L"Unknown card", L"未知指令");
    }
}

const wchar_t* CommandCardDescription(int card) {
    switch (static_cast<stellar::CommandCardId>(card)) {
    case stellar::CommandCardId::kEmergencyLevy:
        return T(L"+5 ships, -3 pop, -8 morale", L"+5 舰，-3 人口，-8 士气");
    case stellar::CommandCardId::kResearchBreakthrough:
        return T(L"+150 research, -80 power", L"+150 科研，-80 能源");
    case stellar::CommandCardId::kColonySubsidy:
        return T(L"+220 minerals, +100 alloy, -60 food", L"+220 矿，+100 合金，-60 食物");
    case stellar::CommandCardId::kEmergencyLoan:
        return T(L"+260 ST, -10 morale, -3 reputation", L"+260 ST，-10 士气，-3 信誉");
    case stellar::CommandCardId::kTalentIntake:
        return T(L"+3 pop, -60 research", L"+3 人口，-60 科研");
    case stellar::CommandCardId::kDiplomaticRound:
        return T(L"+90 influence, -90 alloy", L"+90 影响力，-90 合金");
    case stellar::CommandCardId::kBlackMarketRun:
        return T(L"+1 data core, -100 ST", L"+1 数据核心，-100 ST");
    case stellar::CommandCardId::kSilenceListening:
        return T(L"+1 data core, -60 power, -4 morale", L"+1 数据核心，-60 能源，-4 士气");
    case stellar::CommandCardId::kCount:
    default:
        return L"";
    }
}

// ---------------------------------------------------------------------------
// 主程序
// ---------------------------------------------------------------------------

class MonitorApp {
public:
    int Run(HINSTANCE instance_handle, int) {
        instance_handle_ = instance_handle;
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
        app_config_ = LoadAppConfig();
        g_language = app_config_.language;
        ParseStartupCommandLine();
        if (self_test_) {
            return RunSelfTest();
        }
        SaveAppConfig(app_config_);

        RegisterWindowClasses();

        controller_window_ = CreateWindowExW(0, kControllerClassName,
                                             L"Stellar Lord Controller", WS_OVERLAPPEDWINDOW,
                                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                             CW_USEDEFAULT, nullptr, nullptr, instance_handle_,
                                             this);
        if (controller_window_ == nullptr) {
            return 1;
        }

        ShowWindow(controller_window_, SW_HIDE);
        UpdateWindow(controller_window_);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    void ParseStartupCommandLine() {
        int argument_count = 0;
        LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
        if (arguments == nullptr) {
            return;
        }
        for (int i = 1; i < argument_count; ++i) {
            std::wstring argument = arguments[i];
            const std::wstring lower = [&argument] {
                std::wstring result = argument;
                for (wchar_t& ch : result) {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                return result;
            }();
            if (lower == L"--server" || lower == L"/server") {
                startup_server_ = true;
            } else if (lower == L"--client" || lower == L"/client") {
                startup_client_ = true;
            } else if (lower.rfind(L"--port=", 0) == 0) {
                const int port = std::atoi(WideToUtf8(argument.substr(7)).c_str());
                if (port > 0 && port <= 65535) {
                    app_config_.server_port = static_cast<unsigned int>(port);
                }
            } else if (lower.rfind(L"--invite=", 0) == 0) {
                startup_invite_text_ = argument.substr(9);
            } else if (lower == L"--self-test" || lower == L"/self-test") {
                self_test_ = true;
            } else if (argument.find(L"stellar://") != std::wstring::npos ||
                       lower.find(L"room=") != std::wstring::npos) {
                startup_invite_text_ = argument;
            }
        }
        LocalFree(arguments);
        if (!startup_invite_text_.empty()) {
            startup_client_ = true;
            startup_server_ = false;
        }
    }

    // 无窗口联机自检：启动内嵌服务器，用真实 HTTP 请求跑一遍
    // 建房 -> 加入 -> 开局 -> 操作 -> 退出 -> 原 ID 回档 -> 迟到追赶。
    // 结果写入当前目录 selftest_report.txt，退出码 0/1。整个过程不创建任何窗口。
    int RunSelfTest() {
        std::ofstream report("selftest_report.txt", std::ios::trunc);
        int failures = 0;
        auto check = [&](bool ok, const std::string& name, const std::string& detail = {}) {
            report << (ok ? "PASS " : "FAIL ") << name;
            if (!detail.empty()) {
                report << "  [" << detail << "]";
            }
            report << "\n";
            report.flush();
            if (!ok) {
                ++failures;
            }
        };
        auto detail_response = [](const NetResponse& response) {
            if (!response.transport_ok) {
                return response.error;
            }
            return "status=" + std::to_string(response.status) + " body=" + response.body;
        };

        report << "protocol_version=" << rift_server::kProtocolVersion << "\n";

        unsigned short port = 0;
        std::string start_error;
        for (int attempt = 0; attempt < 30 && port == 0; ++attempt) {
            const unsigned short candidate = static_cast<unsigned short>(
                18000 + (GetCurrentProcessId() % 1000) + attempt);
            if (rift_server::Start(candidate, &start_error)) {
                port = candidate;
            }
        }
        if (port == 0) {
            check(false, "embedded server starts", start_error);
            report << "RESULT: FAIL\n";
            return 1;
        }
        report << "port=" << port << "\n";
        struct ServerGuard {
            ~ServerGuard() { rift_server::Stop(); }
        } server_guard;

        const std::string version = std::string("v=") + std::to_string(rift_server::kProtocolVersion);
        auto post = [&](const std::string& path, const std::string& form) {
            std::string body = form;
            if (!body.empty()) {
                body += "&";
            }
            body += version;
            return HttpPostForm("127.0.0.1", port, path, body, 5000);
        };
        auto get = [&](const std::string& path) {
            const std::string separator = path.find('?') == std::string::npos ? "?" : "&";
            return HttpGet("127.0.0.1", port, path + separator + version, 5000);
        };
        auto passed = [](const NetResponse& response) {
            return response.transport_ok && response.status == 200 &&
                   NetField(response.body, "ok") == "1";
        };
        auto read_state = [](const std::string& body, stellar::GameState& state) {
            std::string text;
            for (const std::string& line : NetFields(body, "s")) {
                text += line;
                text.push_back('\n');
            }
            return !text.empty() && stellar::DeserializeState(text, state);
        };

        // 版本不一致必须拒绝，避免不同协议互相把状态写坏。
        const NetResponse old_version = HttpPostForm(
            "127.0.0.1", port, "/api/room/create", "id=selftest_old&name=Old", 5000);
        check(NetField(old_version.body, "err") == "version",
              "old protocol version rejected", detail_response(old_version));

        const NetResponse create =
            post("/api/room/create", "id=selftest_host&name=Host");
        check(passed(create), "room created", detail_response(create));
        const std::string room = NetField(create.body, "room");
        check(!room.empty(), "room code returned", room);
        check(NetField(create.body, "v") == std::to_string(rift_server::kProtocolVersion),
              "server advertises protocol version", NetField(create.body, "v"));

        const NetResponse list = get("/api/room/list");
        bool listed = false;
        for (const std::string& entry : NetFields(list.body, "room")) {
            if (entry.rfind(room + "|", 0) == 0) {
                listed = true;
            }
        }
        check(listed, "waiting room is listed", detail_response(list));

        // 邀请链接解析/生成（不依赖任何窗口）。
        InviteInfo invite;
        std::wstring invite_error;
        check(ParseInviteText(L"stellar://join?host=192.168.1.5&port=8787&room=ABCD", invite,
                              &invite_error) &&
                  invite.host == "192.168.1.5" && invite.port == 8787 &&
                  invite.room == "ABCD",
              "stellar invite link parses", WideToUtf8(invite_error));
        check(ParseInviteText(L"http://192.168.1.5:8787/join/ABCD", invite, &invite_error) &&
                  invite.host == "192.168.1.5" && invite.port == 8787 &&
                  invite.room == "ABCD",
              "http invite link parses", WideToUtf8(invite_error));
        check(ParseInviteText(L"ABCD", invite, &invite_error) && invite.host.empty() &&
                  invite.room == "ABCD",
              "plain room code parses", WideToUtf8(invite_error));
        const std::wstring generated =
            BuildInviteLink("192.168.1.5", 8787, "ABCD");
        check(ParseInviteText(generated, invite, &invite_error) &&
                  invite.host == "192.168.1.5" && invite.port == 8787 &&
                  invite.room == "ABCD",
              "generated invite link round-trips", WideToUtf8(generated));

        const NetResponse join = post(
            "/api/room/join",
            "id=selftest_client&name=Client&room=" + UrlEncode(room));
        check(passed(join), "client joins before start", detail_response(join));

        const NetResponse start = post(
            "/api/room/start", "id=selftest_host&room=" + UrlEncode(room));
        check(passed(start), "host starts the match", detail_response(start));

        const NetResponse host_state = get(
            "/api/room/state?id=selftest_host&room=" + UrlEncode(room));
        stellar::GameState parsed_host;
        check(passed(host_state) && read_state(host_state.body, parsed_host),
              "host receives serialized state", detail_response(host_state));

        const NetResponse build = post(
            "/api/room/action",
            "id=selftest_client&room=" + UrlEncode(room) + "&action=build&arg=0");
        check(passed(build), "client action accepted", detail_response(build));

        const NetResponse client_state = get(
            "/api/room/state?id=selftest_client&room=" + UrlEncode(room));
        stellar::GameState parsed_client;
        const bool client_state_ok =
            passed(client_state) && read_state(client_state.body, parsed_client);
        check(client_state_ok, "client receives state", detail_response(client_state));
        check(client_state_ok &&
                  parsed_client.buildings[static_cast<int>(stellar::BuildingId::kPower)] >= 2,
              "client build is server-authoritative",
              client_state_ok ? std::to_string(parsed_client.buildings[0]) : "");

        const std::string audit_text = NetField(client_state.body, "audit");
        const double audit_before = std::atof(audit_text.c_str());
        check(!audit_text.empty() && audit_before > 0.0,
              "room exposes shared audit pressure", audit_text);
        const NetResponse relief = post(
            "/api/room/action",
            "id=selftest_client&room=" + UrlEncode(room) + "&action=audit_relief&arg=1");
        check(passed(relief), "audit relief accepted", detail_response(relief));
        const NetResponse audit_state = get(
            "/api/room/state?id=selftest_client&room=" + UrlEncode(room));
        const double audit_after = std::atof(NetField(audit_state.body, "audit").c_str());
        check(passed(audit_state) && audit_after < audit_before,
              "audit relief lowers shared pressure",
              passed(audit_state) ? std::to_string(audit_before) + " -> " +
                                        std::to_string(audit_after)
                                  : detail_response(audit_state));

        // 贸易：卖 50 矿物换 ST，同时确认市场/合同/黑市都已经生成。
        const NetResponse sell = post(
            "/api/room/action",
            "id=selftest_client&room=" + UrlEncode(room) + "&action=sell&arg=1:20");
        check(passed(sell), "market sell accepted", detail_response(sell));
        const NetResponse market_state = get(
            "/api/room/state?id=selftest_client&room=" + UrlEncode(room));
        stellar::GameState market;
        const bool market_ok = passed(market_state) && read_state(market_state.body, market);
        check(market_ok && market.tokens > 0.0,
              "market sell grants ST",
              market_ok ? std::to_string(market.tokens) : detail_response(market_state));
        check(market_ok &&
                  market.contracts.size() ==
                      static_cast<std::size_t>(stellar::kMaxContracts) &&
                  market.black_market.size() ==
                      static_cast<std::size_t>(stellar::kMaxBlackMarketOffers),
              "market generates contracts and black market stock",
              market_ok ? std::to_string(market.contracts.size()) + "/" +
                              std::to_string(market.black_market.size())
                        : "");

        // 远征：从 10 艘舰队里抽 4 艘去未探索扇区，服务端保存航行中的队伍。
        const NetResponse expedition = post(
            "/api/room/action",
            "id=selftest_client&room=" + UrlEncode(room) + "&action=expedition&arg=0:0");
        check(passed(expedition), "expedition launch accepted", detail_response(expedition));
        const NetResponse expedition_state = get(
            "/api/room/state?id=selftest_client&room=" + UrlEncode(room));
        stellar::GameState expedition_parsed;
        const bool expedition_ok =
            passed(expedition_state) &&
            read_state(expedition_state.body, expedition_parsed);
        check(expedition_ok && expedition_parsed.expeditions.size() == 1 &&
                  expedition_parsed.fleet == 6,
              "expedition commits fleet on the server",
              expedition_ok ? "fleet=" + std::to_string(expedition_parsed.fleet) +
                                  " expeditions=" +
                                  std::to_string(expedition_parsed.expeditions.size())
                            : detail_response(expedition_state));

        const NetResponse faction = post(
            "/api/room/action",
            "id=selftest_client&room=" + UrlEncode(room) + "&action=faction&arg=2");
        check(passed(faction), "doctrine action accepted", detail_response(faction));

        const NetResponse leave = post(
            "/api/room/leave",
            "id=selftest_client&room=" + UrlEncode(room));
        check(passed(leave), "client leaves", detail_response(leave));

        const NetResponse rejoin = post(
            "/api/room/join",
            "id=selftest_client&name=Client&room=" + UrlEncode(room));
        check(passed(rejoin), "same client id rejoins", detail_response(rejoin));

        const NetResponse restored_state = get(
            "/api/room/state?id=selftest_client&room=" + UrlEncode(room));
        stellar::GameState restored;
        const bool restored_ok =
            passed(restored_state) && read_state(restored_state.body, restored);
        check(restored_ok, "rejoined client receives state",
              detail_response(restored_state));
        check(restored_ok &&
                  restored.buildings[static_cast<int>(stellar::BuildingId::kPower)] >= 2 &&
                  restored.faction == static_cast<int>(stellar::FactionId::kChoir) &&
                  restored.tokens > 0.0 && restored.expeditions.size() == 1,
              "rejoined client keeps previous progress",
              restored_ok ? "power=" + std::to_string(restored.buildings[0]) +
                                " faction=" + std::to_string(restored.faction) +
                                " ST=" + std::to_string(restored.tokens) +
                                " expeditions=" +
                                std::to_string(restored.expeditions.size())
                          : "");

        Sleep(2500);
        const NetResponse late_join = post(
            "/api/room/join",
            "id=selftest_late&name=Late&room=" + UrlEncode(room));
        check(passed(late_join), "late client joins started room", detail_response(late_join));
        const NetResponse late_state = get(
            "/api/room/state?id=selftest_late&room=" + UrlEncode(room));
        stellar::GameState late;
        const bool late_ok = passed(late_state) && read_state(late_state.body, late);
        check(late_ok && late.play_seconds >= 1.0, "late client gets catch-up time",
              late_ok ? std::to_string(late.play_seconds) : detail_response(late_state));

        post("/api/room/leave", "id=selftest_client&room=" + UrlEncode(room));
        post("/api/room/leave", "id=selftest_late&room=" + UrlEncode(room));
        post("/api/room/leave", "id=selftest_host&room=" + UrlEncode(room));

        report << (failures == 0 ? "RESULT: PASS\n" : "RESULT: FAIL\n");
        report.flush();
        return failures == 0 ? 0 : 1;
    }

    void RegisterWindowClasses() const {
        HICON app_icon = LoadIconW(instance_handle_, MAKEINTRESOURCEW(IDI_APP_ICON));

        WNDCLASSEXW controller_class{};
        controller_class.cbSize = sizeof(controller_class);
        controller_class.lpfnWndProc = &MonitorApp::ControllerWindowProc;
        controller_class.hInstance = instance_handle_;
        controller_class.lpszClassName = kControllerClassName;
        controller_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        controller_class.hIcon = app_icon;
        controller_class.hIconSm = app_icon;
        RegisterClassExW(&controller_class);

        WNDCLASSEXW widget_class{};
        widget_class.cbSize = sizeof(widget_class);
        widget_class.lpfnWndProc = &MonitorApp::WidgetWindowProc;
        widget_class.hInstance = instance_handle_;
        widget_class.lpszClassName = kWidgetClassName;
        widget_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        widget_class.hIcon = app_icon;
        widget_class.hIconSm = app_icon;
        RegisterClassExW(&widget_class);

        WNDCLASSEXW popup_class{};
        popup_class.cbSize = sizeof(popup_class);
        popup_class.lpfnWndProc = &MonitorApp::PopupWindowProc;
        popup_class.hInstance = instance_handle_;
        popup_class.lpszClassName = kPopupClassName;
        popup_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        popup_class.hIcon = app_icon;
        popup_class.hIconSm = app_icon;
        RegisterClassExW(&popup_class);

        WNDCLASSEXW server_class{};
        server_class.cbSize = sizeof(server_class);
        server_class.lpfnWndProc = &MonitorApp::ServerWindowProc;
        server_class.hInstance = instance_handle_;
        server_class.lpszClassName = kServerClassName;
        server_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        server_class.hIcon = app_icon;
        server_class.hIconSm = app_icon;
        server_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&server_class);

        WNDCLASSEXW room_class{};
        room_class.cbSize = sizeof(room_class);
        room_class.lpfnWndProc = &MonitorApp::RoomWindowProc;
        room_class.hInstance = instance_handle_;
        room_class.lpszClassName = kRoomClassName;
        room_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        room_class.hIcon = app_icon;
        room_class.hIconSm = app_icon;
        room_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&room_class);

        WNDCLASSEXW launch_class{};
        launch_class.cbSize = sizeof(launch_class);
        launch_class.lpfnWndProc = &MonitorApp::LaunchWindowProc;
        launch_class.hInstance = instance_handle_;
        launch_class.lpszClassName = kLaunchClassName;
        launch_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        launch_class.hIcon = app_icon;
        launch_class.hIconSm = app_icon;
        launch_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&launch_class);

        WNDCLASSEXW host_class{};
        host_class.cbSize = sizeof(host_class);
        host_class.lpfnWndProc = &MonitorApp::HostWindowProc;
        host_class.hInstance = instance_handle_;
        host_class.lpszClassName = kHostClassName;
        host_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        host_class.hIcon = app_icon;
        host_class.hIconSm = app_icon;
        host_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&host_class);
    }

    bool Initialize() {
        LoadGame();
        StartSamplers();
        freight_mode_ = app_config_.mode == AppMode::kLord;
        UpdateMonitorDisplayLines(last_snapshot_);
        UpdateFreightDisplayLines();
        if (!EnsureWidgetWindow()) {
            return false;
        }
        embedder_.SetTargetMonitorIndex(app_config_.taskbar_monitor_index);
        RefreshFontAndSize();

        ReattachWidget();
        EnsureTrayIcon();

        SetTimer(controller_window_, kSampleTimerId,
                 GetSampleTimerIntervalMs(app_config_.sample_interval_seconds), nullptr);
        SetTimer(controller_window_, kLayoutTimerId, kLayoutIntervalMs, nullptr);
        SetTimer(controller_window_, kGameTickTimerId, kGameTickIntervalMs, nullptr);
        RegisterToggleHotkey();
        SampleAndRefresh();
        BeginStartupFlow();
        return true;
    }

    void BeginStartupFlow() {
        if (startup_server_) {
            StartHostMode(false);
            return;
        }
        if (startup_client_) {
            // 从邀请链接直接启动时，把内容预填进加入窗口；其余情况等待玩家粘贴。
            ShowRoomWindow(startup_invite_text_);
            if (!startup_invite_text_.empty()) {
                SetForegroundWindow(room_window_);
                SetFocus(room_code_edit_);
            }
            return;
        }
        ShowLaunchWindow();
    }

    // -- 采样 ---------------------------------------------------------------

    void StartSamplers() {
        const HWND controller = controller_window_;
        struct Samplers {
            SystemMetrics metrics;
            std::unique_ptr<ProcessMonitor> processes;
        };
        system_worker_.Start(
            [samplers = std::shared_ptr<Samplers>{}](const bool& include_processes,
                                                    const auto& cancelled) mutable {
                if (!samplers) samplers = std::make_shared<Samplers>();
                SystemSampleResult result{samplers->metrics.Sample(), std::nullopt};
                if (include_processes && !cancelled()) {
                    if (!samplers->processes) {
                        samplers->processes = std::make_unique<ProcessMonitor>();
                    }
                    result.processes = samplers->processes->Sample(result.metrics, 24);
                }
                return result;
            },
            [controller] { PostMessageW(controller, kSystemSampleReadyMessage, 0, 0); });
    }

    void ReceiveSystemSample() {
        auto result = system_worker_.TakeResult();
        if (!result || is_shutting_down_) {
            return;
        }
        last_snapshot_ = result->metrics;
        if (!freight_mode_) {
            UpdateMonitorDisplayLines(last_snapshot_);
            RequestWidgetRedraw();
        }
        if (result->processes) {
            process_snapshot_ = std::move(*result->processes);
            if (popup_visible_ && !freight_mode_) {
                const SIZE size_before = hover_popup_size_;
                UpdatePopupSize();
                if (hover_popup_size_.cy != size_before.cy ||
                    hover_popup_size_.cx != size_before.cx) {
                    PositionPopup();
                }
                RequestPopupRedraw();
            }
        }
    }

    void SampleAndRefresh() {
        if (freight_mode_) {
            return;
        }
        system_worker_.Submit(popup_visible_ ? L"processes" : L"metrics", popup_visible_);
    }

    // -- 窗口 ---------------------------------------------------------------

    bool EnsureWidgetWindow() {
        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            return true;
        }
        widget_window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                         kWidgetClassName, L"Stellar Lord Widget", WS_POPUP, 0,
                                         0, 0, 0, nullptr, nullptr, instance_handle_, this);
        return widget_window_ != nullptr;
    }

    bool EnsurePopupWindow() {
        if (popup_window_ != nullptr && IsWindow(popup_window_)) {
            return true;
        }
        popup_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPopupClassName,
                                        L"Stellar Lord Popup", WS_POPUP, 0, 0, 0, 0, nullptr,
                                        nullptr, instance_handle_, this);
        return popup_window_ != nullptr;
    }

    bool EnsureTrayIcon() {
        if (tray_icon_added_ || controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return tray_icon_added_;
        }
        if (tray_icon_handle_ == nullptr) {
            tray_icon_handle_ = LoadIconW(instance_handle_, MAKEINTRESOURCEW(IDI_APP_ICON));
        }
        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        notify_icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        notify_icon.uCallbackMessage = kTrayIconCallbackMessage;
        notify_icon.hIcon = tray_icon_handle_;
        wcscpy_s(notify_icon.szTip, T(L"System Monitor", L"系统监视器"));
        tray_icon_added_ = Shell_NotifyIconW(NIM_ADD, &notify_icon) != FALSE;
        return tray_icon_added_;
    }

    void RemoveTrayIcon() {
        if (!tray_icon_added_) {
            return;
        }
        NOTIFYICONDATAW notify_icon{};
        notify_icon.cbSize = sizeof(notify_icon);
        notify_icon.hWnd = controller_window_;
        notify_icon.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &notify_icon);
        tray_icon_added_ = false;
    }

    void Shutdown() {
        is_shutting_down_ = true;
        system_worker_.Stop();
        KillTimer(controller_window_, kSampleTimerId);
        KillTimer(controller_window_, kLayoutTimerId);
        KillTimer(controller_window_, kReattachTimerId);
        KillTimer(controller_window_, kHoverHideTimerId);
        KillTimer(controller_window_, kGameTickTimerId);
        UnregisterHotKey(controller_window_, kToggleModeHotkeyId);
        HidePopup();
        RemoveTrayIcon();
        embedder_.Detach(widget_window_);

        // 服务器设置窗口的子控件引用了 popup_font_，必须在删除字体之前销毁。
        if (server_window_ != nullptr && IsWindow(server_window_)) {
            DestroyWindow(server_window_);
            server_window_ = nullptr;
        }
        if (room_window_ != nullptr && IsWindow(room_window_)) {
            DestroyWindow(room_window_);
            room_window_ = nullptr;
        }
        if (launch_window_ != nullptr && IsWindow(launch_window_)) {
            DestroyWindow(launch_window_);
            launch_window_ = nullptr;
        }
        if (host_window_ != nullptr && IsWindow(host_window_)) {
            DestroyWindow(host_window_);
            host_window_ = nullptr;
        }
        rift_server::Stop();

        if (font_ != nullptr) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (popup_font_ != nullptr) {
            DeleteObject(popup_font_);
            popup_font_ = nullptr;
        }
        if (popup_title_font_ != nullptr) {
            DeleteObject(popup_title_font_);
            popup_title_font_ = nullptr;
        }
        if (widget_window_ != nullptr && IsWindow(widget_window_)) {
            DestroyWindow(widget_window_);
            widget_window_ = nullptr;
        }
        if (popup_window_ != nullptr && IsWindow(popup_window_)) {
            DestroyWindow(popup_window_);
            popup_window_ = nullptr;
        }
    }

    void ReattachWidget(bool recreate_widget = false) {
        HidePopup();
        if (recreate_widget && widget_window_ != nullptr && IsWindow(widget_window_)) {
            HWND old_widget = widget_window_;
            widget_window_ = nullptr;
            embedder_.Detach(old_widget);
            DestroyWindow(old_widget);
        }
        if (!EnsureWidgetWindow()) {
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }
        embedder_.Detach(widget_window_);
        if (!embedder_.Attach(widget_window_)) {
            if (widget_window_ != nullptr && IsWindow(widget_window_)) {
                DestroyWindow(widget_window_);
                widget_window_ = nullptr;
            }
            if (!EnsureWidgetWindow() || !embedder_.Attach(widget_window_)) {
                if (widget_window_ != nullptr) {
                    ShowWindow(widget_window_, SW_HIDE);
                }
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                return;
            }
        }
        RefreshFontAndSize();
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            return;
        }
        ShowWindow(widget_window_, SW_SHOWNOACTIVATE);
        RequestWidgetRedraw();
    }

    void ScheduleTaskbarReattach() {
        HidePopup();
        embedder_.Detach(widget_window_);
        SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
    }

    // -- 字体与尺寸 ---------------------------------------------------------

    void RefreshFontAndSize() {
        const UINT dpi = embedder_.CurrentDpi();
        if (dpi != current_dpi_ || font_ == nullptr || popup_font_ == nullptr ||
            popup_title_font_ == nullptr) {
            current_dpi_ = dpi;
            if (font_ != nullptr) DeleteObject(font_);
            if (popup_font_ != nullptr) DeleteObject(popup_font_);
            if (popup_title_font_ != nullptr) DeleteObject(popup_title_font_);
            font_ = CreatePreferredUiFont(current_dpi_, 12);
            popup_font_ = CreatePreferredUiFont(current_dpi_, 13);
            popup_title_font_ = CreatePreferredUiFont(current_dpi_, 15);
        }

        HDC screen_dc = GetDC(nullptr);
        HFONT active_font = font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(screen_dc, active_font);
        TEXTMETRICW text_metrics{};
        GetTextMetricsW(screen_dc, &text_metrics);

        // 必须在控件字体（active_font）仍被选中时测量，否则拿到的是弹窗字体的宽度。
        const SIZE line1_size = MeasureTextSize(screen_dc, widget_line1_);
        const SIZE line2_size = MeasureTextSize(screen_dc, widget_line2_);
        // 货运模式的数字会随产量增长（999 → 1.23k → ...），按预留宽度测一次，
        // 免得任务栏控件每次进位都变宽一点。预留只覆盖三位数，避免控件留太多空白。
        const SIZE reserved_freight_line1 =
            freight_mode_ ? MeasureTextSize(screen_dc, L"ORE 999   ALY 999   PRT 999") : SIZE{};
        const SIZE reserved_freight_line2 =
            freight_mode_ ? MeasureTextSize(screen_dc, L"CRG 999   CR 999   SHIP 8/8") : SIZE{};

        // 控件按列绘制（见 DrawWidget），列与列之间有 column_gap，
        // 所以整行宽度不等于 line1/line2 字符串的宽度——要按列分别累加。
        // 这里用「样本行」（CPU 100% / ↑ 999Mbps 等）测宽，而不是当前实时值：
        // 否则 9% → 10% 这类位数变化会让任务栏控件每秒抖动一次。
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        int columns_width = 0;
        widget_column_widths_.clear();
        if (!widget_columns_.empty()) {
            const DisplayLines sample_lines = GetMetricsSampleLines(
                app_config_.visible_metrics, app_config_.network_display_unit,
                g_language == UiLanguage::kChinese);
            if (sample_lines.columns.size() == widget_columns_.size()) {
                widget_column_widths_ = MeasureColumnWidths(screen_dc, sample_lines.columns);
                // 极端值（比如 1023Mbps）可能比样本更宽，取两者的最大值兜底。
                const std::vector<int> actual_widths =
                    MeasureColumnWidths(screen_dc, widget_columns_);
                for (std::size_t i = 0; i < widget_column_widths_.size(); ++i) {
                    widget_column_widths_[i] =
                        std::max(widget_column_widths_[i], actual_widths[i]);
                }
            } else {
                widget_column_widths_ = MeasureColumnWidths(screen_dc, widget_columns_);
            }
            columns_width = SumColumnWidths(widget_column_widths_, column_gap);
        }

        HFONT popup_font = popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_font);
        TEXTMETRICW popup_metrics{};
        GetTextMetricsW(screen_dc, &popup_metrics);

        HFONT popup_title_font =
            popup_title_font_ != nullptr ? popup_title_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SelectObject(screen_dc, popup_title_font);
        TEXTMETRICW title_metrics{};
        GetTextMetricsW(screen_dc, &title_metrics);

        SelectObject(screen_dc, old_font);
        ReleaseDC(nullptr, screen_dc);

        text_line_height_ = text_metrics.tmHeight;
        popup_text_line_height_ = popup_metrics.tmHeight;
        popup_title_line_height_ = title_metrics.tmHeight;

        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 5);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const bool has_second_line = !widget_line2_.empty();
        int content_width =
            std::max<int>({static_cast<int>(line1_size.cx), static_cast<int>(line2_size.cx),
                           columns_width});
        content_width = std::max<int>(content_width, static_cast<int>(reserved_freight_line1.cx));
        content_width = std::max<int>(content_width, static_cast<int>(reserved_freight_line2.cx));

        widget_size_.cx =
            std::max<int>(ScaleByDpi(current_dpi_, 150), content_width + horizontal_padding * 2);
        widget_size_.cy =
            std::max<int>((has_second_line ? text_line_height_ * 2 + line_gap : text_line_height_) +
                              vertical_padding * 2,
                          ScaleByDpi(current_dpi_, has_second_line ? 32 : 24));
        has_second_line_ = has_second_line;
        UpdatePopupSize();
    }

    // -- 显示文本 -----------------------------------------------------------

    void UpdateMonitorDisplayLines(const MetricsSnapshot& snapshot) {
        const DisplayLines lines = FormatMetricsLines(snapshot, app_config_.visible_metrics,
                                                      app_config_.network_display_unit,
                                                      g_language == UiLanguage::kChinese);
        monitor_line1_ = lines.line1;
        monitor_line2_ = lines.line2;
        monitor_columns_ = lines.columns;
        ApplyWidgetLines();
    }

    void UpdateFreightDisplayLines() {
        const auto& state = game_;
        freight_line1_ = L"PWR " + FormatCompact(state.energy) + L"   MIN " +
                         FormatCompact(state.minerals) + L"   FOOD " + FormatCompact(state.food) +
                         L"   ST " + FormatCompact(state.tokens);
        freight_line2_ = L"POP " + FormatCompact(state.population) + L"/" +
                         FormatCompact(stellar::PopulationCapacity(state)) + L"   ALY " +
                         FormatCompact(state.alloy) + L"   FLT " + std::to_wstring(state.fleet) +
                         L"   MOR " + FormatCompact(state.morale);
        ApplyWidgetLines();
    }

    void ApplyWidgetLines() {
        if (freight_mode_) {
            widget_columns_.clear();
            widget_line1_ = freight_line1_;
            widget_line2_ = freight_line2_;
        } else {
            widget_line1_ = monitor_line1_;
            widget_line2_ = monitor_line2_;
            widget_columns_ = monitor_columns_;
        }
    }

    // -- 游戏 ---------------------------------------------------------------

    void LoadGame() {
        // 反作弊：进度不落盘，只在内存里。每次启动都是新的平民身份；
        // 联机房间里的状态由服务端保存并结算。
        game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
        if (!app_config_.nickname.empty()) {
            std::wstring nick = TrimWide(Utf8ToWide(app_config_.nickname));
            if (nick.size() > 16) {
                nick.resize(16);
            }
            player_name_ = WideToUtf8(nick);
        }
        if (player_name_.empty()) {
            player_name_ = "Lord";
        }
        if (player_name_ == "Lord" && app_config_.player_id.size() >= 4) {
            player_name_ += "-" + app_config_.player_id.substr(app_config_.player_id.size() - 4);
        }
    }

    void TickGame() {
        if (is_shutting_down_) {
            return;
        }
        const ULONGLONG now_ms = GetTickCount64();
        double dt = 1.0;
        if (last_tick_ms_ != 0 && now_ms > last_tick_ms_) {
            dt = static_cast<double>(now_ms - last_tick_ms_) / 1000.0;
        }
        last_tick_ms_ = now_ms;
        dt = std::min(dt, 60.0);

        if (!room_active_) {
            stellar::Advance(game_, dt);
        } else if (now_ms - last_room_poll_ms_ >= kRoomPollIntervalMs &&
                   !room_poll_pending_) {
            last_room_poll_ms_ = now_ms;
            RequestRoomState();
        }

        if (freight_mode_) {
            UpdateFreightDisplayLines();
            RefreshFontAndSize();
            if (embedder_.IsAttached()) {
                embedder_.RefreshLayout(widget_window_, widget_size_);
            }
            RequestWidgetRedraw();
        }
        if (popup_visible_ && freight_mode_) {
            const SIZE size_before = hover_popup_size_;
            RefreshFreightRows();
            if (hover_popup_size_.cy != size_before.cy || hover_popup_size_.cx != size_before.cx) {
                PositionPopup();
            }
            RequestPopupRedraw();
        }
    }

    void ResetProgress() {
        if (room_active_) {
            MessageBoxW(controller_window_,
                        T(L"Leave the rift before resetting. The room is server-authoritative.",
                          L"联机对局由服务端结算，请先离开时空再重置。"),
                        T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONINFORMATION);
            return;
        }
        const int answer = MessageBoxW(controller_window_,
                                       T(L"Reset your empire and start over as a commoner?",
                                         L"确定要重置帝国，重新从平民开始吗？"),
                                       T(L"Stellar Lord", L"星际领主"),
                                       MB_YESNO | MB_ICONQUESTION);
        if (answer != IDYES) {
            return;
        }
        game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
        last_tick_ms_ = GetTickCount64();
        UpdateFreightDisplayLines();
        RefreshFontAndSize();
        RequestWidgetRedraw();
        if (popup_visible_) {
            RefreshFreightRows();
            RequestPopupRedraw();
        }
    }

    void ToggleMode() {
        freight_mode_ = !freight_mode_;
        app_config_.mode = freight_mode_ ? AppMode::kLord : AppMode::kMonitor;
        SaveAppConfig(app_config_);
        HidePopup();
        ApplyWidgetLines();
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            embedder_.RefreshLayout(widget_window_, widget_size_);
        }
        RequestWidgetRedraw();
        if (!freight_mode_) {
            SampleAndRefresh();
        }
    }

    // -- 货运行构建 ---------------------------------------------------------

    void RefreshFreightRows() {
        freight_rows_.clear();
        const auto& state = game_;

        auto add_row = [this](FreightRow row) { freight_rows_.push_back(std::move(row)); };

        auto cost_text = [](const stellar::Cost& cost) {
            std::wstring text;
            auto part = [&text](const wchar_t* tag, double value) {
                if (value <= 0.0001) {
                    return;
                }
                if (!text.empty()) {
                    text += L"  ";
                }
                text += tag;
                text += FormatCompact(value);
            };
            part(L"PWR ", cost.energy);
            part(L"MIN ", cost.minerals);
            part(L"FOOD ", cost.food);
            part(L"ALY ", cost.alloy);
            return text.empty() ? std::wstring(T(L"free", L"免费")) : text;
        };

        auto can_pay = [](const stellar::GameState& value, const stellar::Cost& cost) {
            return value.energy >= cost.energy && value.minerals >= cost.minerals &&
                   value.food >= cost.food && value.alloy >= cost.alloy;
        };

        switch (freight_tab_) {
        case FreightTab::kDevelopment: {
            {
                FreightRow row;
                row.left = T(L"Empire", L"帝国状态");
                row.mid = std::wstring(FactionName(state.faction)) + L"   " +
                          T(L"morale ", L"士气 ") + FormatCompact(state.morale) + L"   " +
                          T(L"score ", L"霸业 ") + FormatCompact(stellar::Score(state));
                if (!state.events.empty()) {
                    row.mid += std::wstring(L"   ") + T(L"events ", L"待办 ") +
                               std::to_wstring(state.events.size());
                }
                row.enabled = false;
                add_row(row);
            }
            const wchar_t* names[6] = {T(L"Power plant", L"电站"),  T(L"Mine", L"矿场"),
                                       T(L"Farm", L"农场"),        T(L"Factory", L"工厂"),
                                       T(L"Laboratory", L"实验室"), T(L"Barracks", L"军营")};
            const wchar_t* rates[6] = {L"PWR", L"MIN", L"FOOD", L"ALY", L"SCI", L"FLT"};
            const stellar::Rates rate = stellar::ComputeRates(state);
            const double values[6] = {rate.energy, rate.minerals, rate.food, rate.alloy,
                                      rate.research, rate.fleet_growth};
            for (int i = 0; i < 6; ++i) {
                const auto id = static_cast<stellar::BuildingId>(i);
                const int level = state.buildings[i];
                FreightRow row;
                row.left = names[i];
                row.mid = L"Lv." + std::to_wstring(level) + L"/" +
                          std::to_wstring(stellar::kMaxBuildingLevel) + L"   " + rates[i] +
                          L" " + FormatW(L"%+.2f/s", values[i]);
                if (level >= stellar::kMaxBuildingLevel) {
                    row.right = T(L"MAX", L"已满");
                    row.enabled = false;
                } else {
                    const stellar::Cost cost = stellar::BuildingUpgradeCost(state, id);
                    row.right = cost_text(cost);
                    row.enabled = stellar::CanBuild(state, id);
                }
                row.action = RowAction::kBuild;
                row.index = i;
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"Capital", L"首都开发");
                row.mid = L"Lv." + std::to_wstring(state.capital_level) + L"/" +
                          std::to_wstring(stellar::kMaxCapitalLevel) + L"   " +
                          T(L"pop cap ", L"人口上限 ") +
                          FormatCompact(stellar::PopulationCapacity(state));
                if (state.capital_level >= stellar::kMaxCapitalLevel) {
                    row.right = T(L"MAX", L"已满");
                    row.enabled = false;
                } else {
                    const stellar::Cost cost = stellar::CapitalUpgradeCost(state);
                    row.right = cost_text(cost);
                    row.enabled = can_pay(state, cost);
                }
                row.action = RowAction::kCapital;
                add_row(row);
            }
            break;
        }
        case FreightTab::kResearch: {
            {
                FreightRow row;
                row.left = T(L"Research pool", L"科研池");
                row.mid = FormatCompact(state.research) + T(L" pts   +", L" 点   +") +
                          FormatW(L"%.2f/s", stellar::ComputeRates(state).research);
                row.enabled = false;
                add_row(row);
            }
            const wchar_t* names[6] = {T(L"Energy grid", L"能源效率"),
                                       T(L"Deep mining", L"采矿效率"),
                                       T(L"Agri science", L"农业与人口"),
                                       T(L"Industry", L"工业效率"),
                                       T(L"Military drill", L"军事训练"),
                                       T(L"Colonization", L"殖民技术")};
            const wchar_t* effects[6] = {T(L"+20% power", L"能源 +20%/级"),
                                         T(L"+20% minerals", L"矿物 +20%/级"),
                                         T(L"+25% growth", L"人口增长 +25%/级"),
                                         T(L"+20% alloy", L"合金 +20%/级"),
                                         T(L"+15% fleet", L"舰队训练 +15%/级"),
                                         T(L"-10% colony cost", L"殖民成本 -10%/级")};
            for (int i = 0; i < 6; ++i) {
                const auto id = static_cast<stellar::TechId>(i);
                const int level = state.techs[i];
                FreightRow row;
                row.left = names[i];
                row.mid = L"Lv." + std::to_wstring(level) + L"/" +
                          std::to_wstring(stellar::kMaxTechLevel) + L"   " + effects[i];
                if (level >= stellar::kMaxTechLevel) {
                    row.right = T(L"MAX", L"已满");
                    row.enabled = false;
                } else {
                    row.right = FormatCompact(stellar::TechCost(state, id)) +
                                T(L" pts", L" 点");
                    row.enabled = stellar::CanBuyTech(state, id);
                }
                row.action = RowAction::kResearchTech;
                row.index = i;
                add_row(row);
            }
            break;
        }
        case FreightTab::kMilitary: {
            const stellar::Rates rate = stellar::ComputeRates(state);
            {
                FreightRow row;
                row.left = T(L"Fleet", L"舰队");
                row.mid = FormatCompact(static_cast<double>(state.fleet)) + L" / " +
                          std::to_wstring(stellar::FleetCapacity(state)) + L"   " +
                          T(L"train +", L"训练 +") + FormatW(L"%.2f/s", rate.fleet_growth);
                row.enabled = false;
                add_row(row);
            }
            for (int count : {5, 20}) {
                FreightRow row;
                row.left = T(L"Build squadron", L"建造舰队");
                const stellar::Cost cost = stellar::FleetBuildCost(state, count);
                row.mid = T(L"+", L"+") + std::to_wstring(count) + T(L" ships   ", L" 艘   ") +
                          cost_text(cost);
                row.right = T(L"Build", L"建造");
                row.action = RowAction::kBuildFleet;
                row.index = count;
                row.enabled = state.buildings[static_cast<int>(stellar::BuildingId::kBarracks)] > 0 &&
                              state.population >= 3.0 + 0.5 * static_cast<double>(count) &&
                              state.fleet + count <= stellar::FleetCapacity(state) &&
                              can_pay(state, cost);
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"Neutral system", L"进攻中立星域");
                const int defense = stellar::NeutralDefense(state);
                row.mid = T(L"defense ", L"防御 ") + std::to_wstring(defense) +
                          T(L"   loot + colony", L"   掠夺 + 殖民地");
                row.right = T(L"Attack", L"进攻");
                row.action = RowAction::kAttackNeutral;
                row.enabled = state.fleet >= 5;
                add_row(row);
            }
            for (int target = 0; target < static_cast<int>(stellar::SectorTargetId::kCount);
                 ++target) {
                const auto target_id = static_cast<stellar::SectorTargetId>(target);
                FreightRow row;
                row.left = target_id == stellar::SectorTargetId::kMiningBelt
                               ? T(L"Mining belt", L"富矿带")
                               : T(L"Silent outpost", L"静默者前哨");
                const int defense = stellar::SectorDefense(state, target_id);
                row.mid = T(L"defense ", L"防御 ") + std::to_wstring(defense) + L"   " +
                          (target_id == stellar::SectorTargetId::kMiningBelt
                               ? T(L"loot MIN + ALY", L"掠夺 矿物 + 合金")
                               : T(L"loot SCI + ALY, morale -4", L"掠夺 科研 + 合金，士气 -4"));
                row.right = T(L"Raid", L"突袭");
                row.action = RowAction::kAttackSector;
                row.index = target;
                row.enabled = state.fleet >= 5;
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"Battle record", L"战绩");
                row.mid = T(L"won ", L"胜 ") + std::to_wstring(state.battles_won) + T(L"   lost ", L"   负 ") +
                          std::to_wstring(state.battles_lost);
                row.enabled = false;
                add_row(row);
            }
            break;
        }
        case FreightTab::kEmpire: {
            for (int wonder = 0; wonder < stellar::kWonderCount; ++wonder) {
                FreightRow row;
                row.left = WonderName(wonder);
                const bool built = stellar::HasWonder(state, wonder);
                if (built) {
                    row.mid = std::wstring(T(L"completed   ", L"已建成   ")) +
                              WonderDescription(wonder);
                    row.right = T(L"Built", L"已建成");
                    row.enabled = false;
                } else {
                    const stellar::Cost cost = stellar::WonderCost(state, wonder);
                    row.mid = std::wstring(WonderDescription(wonder)) + L"   " + cost_text(cost);
                    row.right = T(L"Build", L"建造");
                    row.enabled = stellar::CanBuildWonder(state, wonder);
                    if (wonder == stellar::kWonderKnowledgeArk && state.research < 300.0) {
                        row.mid += std::wstring(L"   ") + T(L"need 300 SCI", L"需要 300 科研");
                    }
                }
                row.action = RowAction::kBuildWonder;
                row.index = wonder;
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"Colonize planet", L"殖民新星球");
                const stellar::Cost cost = stellar::ColonizeCost(state);
                row.mid = T(L"needs 8 pop   ", L"需要人口 8   ") + cost_text(cost);
                row.right = T(L"Colonize", L"殖民");
                row.action = RowAction::kColonize;
                row.enabled = stellar::CanColonize(state);
                add_row(row);
            }
            if (state.colonies.empty()) {
                FreightRow row;
                row.left = T(L"No colonies yet", L"尚未殖民");
                row.mid = T(L"Attack neutrals or colonize to expand.",
                            L"进攻中立星域或殖民来扩张领土。");
                row.enabled = false;
                add_row(row);
            }
            const std::size_t colony_limit = std::min<std::size_t>(state.colonies.size(), 6);
            for (std::size_t i = 0; i < colony_limit; ++i) {
                FreightRow row;
                row.left = T(L"Colony #", L"殖民地 #") + std::to_wstring(i + 1);
                row.mid = L"Lv." + std::to_wstring(state.colonies[i].level) + L"/" +
                          std::to_wstring(stellar::kMaxColonyLevel) + L"   " +
                          T(L"pop cap +", L"人口上限 +") +
                          std::to_wstring(10 * state.colonies[i].level);
                const stellar::Cost cost = stellar::ColonyUpgradeCost(state, i);
                if (state.colonies[i].level >= stellar::kMaxColonyLevel) {
                    row.right = T(L"MAX", L"已满");
                    row.enabled = false;
                } else {
                    row.right = cost_text(cost);
                    row.enabled = can_pay(state, cost);
                }
                row.action = RowAction::kColonyUpgrade;
                row.index = static_cast<int>(i);
                add_row(row);
            }

            if (room_active_ && room_started_) {
                int rivals = 0;
                for (std::size_t i = 0; i < room_members_.size() && rivals < 5; ++i) {
                    const RoomMemberInfo& member = room_members_[i];
                    if (member.me) {
                        continue;
                    }
                    FreightRow row;
                    row.left = T(L"Rival ", L"对手 ") + Utf8ToWide(member.name);
                    row.mid = L"\u2605 " + FormatCompact(member.score) + T(L"   fleet ", L"   舰队 ") +
                              std::to_wstring(member.fleet) + T(L"   colonies ", L"   殖民地 ") +
                              std::to_wstring(member.colonies) + T(L"   ST ", L"   ST ") +
                              FormatCompact(member.tokens) + T(L"   REL ", L"   遗物 ") +
                              std::to_wstring(member.relics);
                    row.has_choice = true;
                    row.choice_a = T(L"Raid", L"劫掠");
                    row.choice_b = T(L"Capture", L"占领");
                    row.choice_a_enabled = state.fleet >= 5;
                    row.choice_b_enabled = state.fleet >= 5;
                    row.action = RowAction::kAttackRival;
                    row.index = static_cast<int>(i);
                    row.enabled = state.fleet >= 5;
                    add_row(row);
                    ++rivals;
                }
                if (rivals == 0) {
                    FreightRow row;
                    row.left = T(L"No rivals online", L"暂无对手");
                    row.mid = T(L"Wait for more lords to join the rift.",
                                L"等待更多领主加入时空。");
                    row.enabled = false;
                    add_row(row);
                }
            }
            break;
        }
        case FreightTab::kTrade: {
            const double next_refresh =
                stellar::kMarketPhaseSeconds -
                std::fmod(std::max(0.0, state.play_seconds), stellar::kMarketPhaseSeconds);
            auto wallet_for = [&state](int resource) -> double {
                switch (resource) {
                case 0: return state.energy;
                case 1: return state.minerals;
                case 2: return state.food;
                case 3: return state.alloy;
                default: return 0.0;
                }
            };

            {
                FreightRow row;
                row.left = T(L"Trade hub", L"贸易港");
                row.mid = T(L"ST ", L"星际代币 ") + FormatCompact(state.tokens) +
                          T(L"   INF ", L"   影响力 ") + FormatCompact(state.influence) +
                          T(L"   REP ", L"   信誉 ") +
                          std::to_wstring(state.reputation) +
                          T(L"   next refresh ", L"   下次刷新 ") +
                          FormatDuration(next_refresh);
                row.enabled = false;
                add_row(row);
            }

            for (int resource = 0; resource < stellar::kTradeResourceCount; ++resource) {
                const double sell_price = stellar::MarketSellPrice(state, resource);
                const double buy_price = stellar::MarketBuyPrice(state, resource);
                const double stock = wallet_for(resource);
                FreightRow row;
                row.left = ResourceName(resource);
                row.mid = T(L"stock ", L"库存 ") + FormatCompact(stock) + L"   " +
                          FormatW(L"%.2f", sell_price) + T(L" ST sell / ", L" ST 卖 / ") +
                          FormatW(L"%.2f", buy_price) + T(L" ST buy", L" ST 买");
                row.has_choice = true;
                row.choice_a = T(L"Sell 50 +", L"卖50 +") +
                               FormatCompact(sell_price * stellar::kTradeLotSize) + L" ST";
                row.choice_b = T(L"Buy 50 -", L"买50 -") +
                               FormatCompact(buy_price * stellar::kTradeLotSize) + L" ST";
                row.choice_a_enabled = stock + 1e-6 >= stellar::kTradeLotSize;
                row.choice_b_enabled =
                    state.tokens + 1e-6 >= buy_price * stellar::kTradeLotSize;
                row.action = RowAction::kTradeResource;
                row.index = resource;
                row.enabled = row.choice_a_enabled || row.choice_b_enabled;
                add_row(row);
            }

            {
                FreightRow row;
                row.left = T(L"Contracts", L"贸易合同");
                row.mid = T(L"deliver goods now, get ST + influence + reputation",
                            L"现在交付货物，换星际代币、影响力和信誉");
                row.enabled = false;
                add_row(row);
            }
            for (std::size_t i = 0; i < state.contracts.size(); ++i) {
                const stellar::TradeContract& contract = state.contracts[i];
                FreightRow row;
                row.left = T(L"Contract #", L"合同 #") + std::to_wstring(i + 1);
                row.mid = T(L"deliver ", L"交付 ") + FormatCompact(contract.amount) + L" " +
                          ResourceName(contract.resource) + L"   \u2192 " +
                          FormatCompact(contract.reward_tokens) + T(L" ST + ", L" ST + ") +
                          FormatCompact(contract.reward_influence) + T(L" INF", L" 影响力");
                if (contract.done) {
                    row.right = T(L"Done", L"已完成");
                    row.enabled = false;
                } else {
                    row.right = T(L"Deliver", L"交付");
                    row.enabled =
                        wallet_for(contract.resource) + 1e-6 >= contract.amount;
                }
                row.action = RowAction::kFulfillContract;
                row.index = static_cast<int>(i);
                add_row(row);
            }

            {
                FreightRow row;
                row.left = T(L"Black market", L"黑市");
                row.mid = T(L"Hecate: prices are alive. Stock refreshes with the market.",
                            L"赫卡忒：价格是活的。库存跟市场一起刷新。");
                row.enabled = false;
                add_row(row);
            }
            for (std::size_t i = 0; i < state.black_market.size(); ++i) {
                const stellar::BlackMarketOffer& offer = state.black_market[i];
                FreightRow row;
                row.left = BlackMarketName(offer.kind);
                row.mid = T(L"stock ", L"库存 ") + std::to_wstring(offer.stock) + L"   " +
                          FormatCompact(offer.price) + L" ST   " +
                          BlackMarketDescription(offer.kind, offer.amount);
                if (offer.stock <= 0) {
                    row.right = T(L"Sold out", L"售罄");
                    row.enabled = false;
                } else {
                    row.right = T(L"Buy", L"购买");
                    row.enabled = state.tokens + 1e-6 >= offer.price;
                }
                row.action = RowAction::kBuyBlackMarket;
                row.index = static_cast<int>(i);
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"Era", L"时代");
                const int era = stellar::EraIndex(state);
                const double next_era = stellar::NextEraIn(state);
                row.mid = std::wstring(EraName(era)) + L"   " +
                          (next_era > 0.0
                               ? T(L"next era in ", L"下一时代还有 ") + FormatDuration(next_era)
                               : std::wstring(T(L"final phase", L"终局阶段")));
                row.enabled = false;
                add_row(row);
            }
            break;
        }
        case FreightTab::kExpedition: {
            const int active = stellar::ActiveExpeditionCount(state);
            int committed = 0;
            for (const stellar::Expedition& expedition : state.expeditions) {
                committed += std::max(0, expedition.fleet);
            }
            {
                FreightRow row;
                row.left = T(L"Expeditions", L"远征队");
                row.mid = std::to_wstring(active) + T(L"/", L"/") +
                          std::to_wstring(stellar::kMaxExpeditions) + T(L" active   ", L" 支在航   ") +
                          T(L"fleet committed ", L"占用舰队 ") + std::to_wstring(committed) +
                          T(L"   free ", L"   可用 ") + std::to_wstring(state.fleet) + L"   " +
                          EraName(stellar::EraIndex(state));
                row.enabled = false;
                add_row(row);
            }

            for (int destination = 0;
                 destination < static_cast<int>(stellar::ExpeditionDestination::kCount);
                 ++destination) {
                const auto destination_id =
                    static_cast<stellar::ExpeditionDestination>(destination);
                const int cautious_cost = stellar::ExpeditionFleetCost(
                    destination_id, stellar::ExpeditionStance::kCautious);
                const int deep_cost = stellar::ExpeditionFleetCost(
                    destination_id, stellar::ExpeditionStance::kDeep);
                FreightRow row;
                row.left = ExpeditionName(destination);
                row.mid = FormatDuration(
                              stellar::ExpeditionDurationSeconds(destination_id)) +
                          L"   " + ExpeditionDescription(destination) + L"   " +
                          T(L"fleet ", L"需要 ") + std::to_wstring(cautious_cost) +
                          T(L" / deep ", L" / 深入 ") + std::to_wstring(deep_cost);
                row.has_choice = true;
                row.choice_a = std::wstring(T(L"Cautious ", L"谨慎 ")) +
                               std::to_wstring(cautious_cost) + T(L" ships", L" 舰");
                row.choice_b = std::wstring(T(L"Deep ", L"深入 ")) +
                               std::to_wstring(deep_cost) + T(L" ships", L" 舰");
                row.choice_a_enabled =
                    active < stellar::kMaxExpeditions && state.fleet >= cautious_cost;
                row.choice_b_enabled =
                    active < stellar::kMaxExpeditions && state.fleet >= deep_cost;
                row.action = RowAction::kLaunchExpedition;
                row.index = destination;
                row.enabled = row.choice_a_enabled || row.choice_b_enabled;
                add_row(row);
            }

            for (const stellar::Expedition& expedition : state.expeditions) {
                FreightRow row;
                row.left = std::wstring(T(L"En route · ", L"航行中 · ")) +
                           ExpeditionName(expedition.destination);
                const double remaining =
                    std::max(0.0, expedition.return_seconds - state.play_seconds);
                row.mid = T(L"returns in ", L"返航还有 ") + FormatDuration(remaining) +
                          L"   " + ExpeditionStanceName(expedition.stance) + L"   " +
                          T(L"fleet ", L"舰队 ") + std::to_wstring(expedition.fleet);
                row.right = T(L"Sailing", L"航行中");
                row.enabled = false;
                add_row(row);
            }

            auto report_summary = [](const stellar::ExpeditionReport& report) {
                std::wstring text = ExpeditionOutcomeName(report.outcome);
                auto add_count = [&text](const wchar_t* tag, int value) {
                    if (value > 0) {
                        text += L"  ";
                        text += tag;
                        text += L" +";
                        text += std::to_wstring(value);
                    }
                };
                add_count(L"REL", report.relics);
                add_count(L"COR", report.cores);
                add_count(L"DATA", report.data_cores);
                if (report.tokens > 0.5) {
                    text += T(L"  ST +", L"  ST +") + FormatCompact(report.tokens);
                }
                if (report.research > 0.5) {
                    text += T(L"  SCI +", L"  科研 +") + FormatCompact(report.research);
                }
                if (report.influence > 0.5) {
                    text += T(L"  INF +", L"  影响力 +") + FormatCompact(report.influence);
                }
                if (report.fleet_loss > 0) {
                    text += T(L"  lost ", L"  损失 ") + std::to_wstring(report.fleet_loss) +
                            T(L" ships", L" 舰");
                }
                return text;
            };

            const std::size_t report_limit = 3;
            std::size_t report_start = 0;
            if (state.expedition_reports.size() > report_limit) {
                report_start = state.expedition_reports.size() - report_limit;
            }
            for (std::size_t i = report_start; i < state.expedition_reports.size(); ++i) {
                const stellar::ExpeditionReport& report = state.expedition_reports[i];
                FreightRow row;
                row.left = std::wstring(T(L"Report · ", L"返航报告 · ")) +
                           ExpeditionName(report.destination);
                row.mid = report_summary(report);
                row.right = T(L"Filed", L"已归档");
                row.enabled = false;
                add_row(row);
            }
            if (!state.expedition_reports.empty()) {
                FreightRow row;
                row.left = T(L"Clear reports", L"清除返航报告");
                row.mid = T(L"keep the expedition log tidy",
                            L"整理远征日志，不清理已获得的收益");
                row.right = T(L"Clear", L"清除");
                row.action = RowAction::kClearExpeditionReports;
                row.enabled = true;
                add_row(row);
            }
            {
                FreightRow row;
                row.left = T(L"ARK-3", L"阿刻");
                row.mid = state.expedition_reports.empty()
                              ? T(L"The result is written at departure. You just have not read it yet.",
                                  L"结果在出发时就写好了。你只是还没读到。")
                              : ExpeditionFlavor(
                                    state.expedition_reports.back().destination,
                                    state.expedition_reports.back().outcome);
                row.enabled = false;
                add_row(row);
            }
            break;
        }
        case FreightTab::kEvents: {
            {
                FreightRow era;
                const int era_index = stellar::EraIndex(state);
                const double next_era = stellar::NextEraIn(state);
                era.left = T(L"Era", L"时代");
                era.mid = std::wstring(EraName(era_index)) + L"   " +
                          (next_era > 0.0
                               ? T(L"next in ", L"下一时代还有 ") + FormatDuration(next_era)
                               : std::wstring(T(L"the rift is closing", L"裂谷正在关闭")));
                era.enabled = false;
                add_row(era);
            }
            {
                FreightRow chronicle;
                chronicle.left = T(L"Chronicle", L"编年史");
                chronicle.mid = EraFlavor(stellar::EraIndex(state));
                chronicle.enabled = false;
                add_row(chronicle);
            }
            if (room_active_ && room_started_) {
                FreightRow row;
                row.left = T(L"Audit", L"审计压力");
                row.mid = T(L"shared by the whole rift   ", L"全房间共享   ") +
                          FormatW(L"%.1f", room_audit_) + L"/100   " +
                          AuditTierName(room_audit_tier_) +
                          T(L"   cost 1 DATA+40P / 30P", L"   代价 数据核心+40电 / 30电") +
                          L"   " + AuditFlavor(room_audit_tier_);
                row.has_choice = true;
                row.choice_a = T(L"Data core -15", L"数据核心 -15");
                row.choice_b = T(L"Broadcast -5", L"静默广播 -5");
                row.choice_a_enabled =
                    room_audit_ > 0.5 && state.data_cores >= 1 && state.energy >= 40.0;
                row.choice_b_enabled = room_audit_ > 0.5 && state.energy >= 30.0;
                row.action = RowAction::kAuditRelief;
                row.enabled = row.choice_a_enabled || row.choice_b_enabled;
                add_row(row);
            }
            if (stellar::CommandCardsAvailable(state)) {
                FreightRow header;
                header.left = T(L"Command cards", L"指令卡");
                header.mid = T(L"choose one now · expires in ", L"现在选一张 · 剩余 ") +
                             FormatDuration(stellar::CommandCardSecondsLeft(state));
                header.enabled = false;
                add_row(header);
                for (int i = 0; i < stellar::kCommandCardChoices; ++i) {
                    const int card = stellar::CommandCardOffer(state, i);
                    FreightRow row;
                    row.left = CommandCardName(card);
                    row.mid = CommandCardDescription(card);
                    row.right = T(L"Take", L"选它");
                    row.action = RowAction::kChooseCommandCard;
                    row.index = i;
                    row.enabled = card >= 0;
                    add_row(row);
                }
            } else {
                FreightRow row;
                row.left = T(L"Command cards", L"指令卡");
                const double until_next =
                    stellar::kMarketPhaseSeconds -
                    std::fmod(std::max(0.0, state.play_seconds),
                              stellar::kMarketPhaseSeconds);
                row.mid = T(L"next offer in ", L"下一批指令还有 ") +
                          FormatDuration(until_next) + T(L"   taken ", L"   已使用 ") +
                          std::to_wstring(state.cards_taken);
                row.enabled = false;
                add_row(row);
            }
            if (state.faction == static_cast<int>(stellar::FactionId::kNone)) {
                FreightRow hint;
                hint.left = T(L"Doctrine", L"治国路线");
                hint.mid = T(L"Pick one route for this run. It is permanent.",
                             L"为这一局选一条路线，选定后不可更改。");
                hint.enabled = false;
                add_row(hint);
                for (int faction = 1; faction < static_cast<int>(stellar::FactionId::kCount);
                     ++faction) {
                    FreightRow row;
                    row.left = FactionName(faction);
                    row.mid = FactionDescription(faction);
                    row.right = T(L"Choose", L"选择");
                    row.action = RowAction::kChooseFaction;
                    row.index = faction;
                    row.enabled = true;
                    add_row(row);
                }
            } else {
                FreightRow current;
                current.left = T(L"Doctrine", L"治国路线");
                current.mid = std::wstring(FactionName(state.faction)) + L"   " +
                              FactionDescription(state.faction);
                current.right = T(L"locked", L"已锁定");
                current.enabled = false;
                add_row(current);
            }

            if (state.events.empty()) {
                FreightRow row;
                row.left = T(L"No pending events", L"暂无待办事件");
                row.mid = T(L"next event in ", L"下一个事件还有 ") +
                          FormatDuration(std::max(0.0, state.next_event_seconds - state.play_seconds));
                row.enabled = false;
                add_row(row);
            } else {
                for (std::size_t i = 0; i < state.events.size(); ++i) {
                    const EventDisplay display = GetEventDisplay(state.events[i].id);
                    FreightRow row;
                    row.left = display.title;
                    row.mid = display.description;
                    row.has_choice = true;
                    row.choice_a = display.choice_a;
                    row.choice_b = display.choice_b;
                    row.choice_a_enabled = true;
                    row.choice_b_enabled = true;
                    row.action = RowAction::kResolveEvent;
                    row.index = static_cast<int>(i);
                    row.enabled = true;
                    add_row(row);
                }
                FreightRow hint;
                hint.left = T(L"Tip", L"提示");
                hint.mid = T(L"Q / E picks the left / right option. Unanswered events are settled conservatively.",
                             L"Q / E 选择左 / 右选项；不处理的事件会按保守方案自动结算。");
                hint.enabled = false;
                add_row(hint);
            }
            break;
        }
        case FreightTab::kOnline:
        default: {
            FreightRow identity;
            identity.left = T(L"My ID", L"我的 ID");
            identity.mid = Utf8ToWide(app_config_.player_id);
            identity.right = T(L"Copy", L"复制");
            identity.action = RowAction::kCopyPlayerId;
            identity.enabled = true;
            add_row(identity);

            if (room_active_) {
                FreightRow invite;
                invite.left = T(L"Invite link", L"邀请链接");
                invite.mid = CurrentInviteLink();
                invite.right = T(L"Copy", L"一键复制");
                invite.action = RowAction::kCopyInviteLink;
                invite.enabled = true;
                add_row(invite);
                if (room_host_) {
                    FreightRow host_panel;
                    host_panel.left = T(L"Host panel", L"主机面板");
                    host_panel.mid = T(L"show the shareable link again",
                                       L"重新显示可分享的邀请链接");
                    host_panel.right = T(L"Open", L"打开");
                    host_panel.action = RowAction::kStartHosting;
                    host_panel.enabled = true;
                    add_row(host_panel);
                }
            }

            FreightRow server;
            server.left = room_host_ ? T(L"Host", L"本机主机") : T(L"Server", L"服务器");
            const std::string shown_host =
                (room_host_ && !host_lan_ip_.empty()) ? host_lan_ip_ : app_config_.server_host;
            server.mid = Utf8ToWide(shown_host) + L":" +
                         std::to_wstring(room_host_ ? host_port_ : app_config_.server_port) +
                         L"   v" + std::to_wstring(rift_server::kProtocolVersion);
            if (!net_status_.empty()) {
                server.mid += L"   " + net_status_;
            }
            server.right = T(L"Edit", L"修改");
            server.action = RowAction::kEditServer;
            server.enabled = true;
            add_row(server);

            if (!room_active_) {
                {
                    FreightRow row;
                    row.left = T(L"Host this PC", L"开启时空（主机）");
                    row.mid = T(L"start the room and copy one invite link",
                                L"创建房间，生成一条可复制的邀请链接");
                    row.right = T(L"Host", L"开启");
                    row.action = RowAction::kStartHosting;
                    row.enabled = true;
                    add_row(row);
                }
                {
                    FreightRow row;
                    row.left = T(L"Join rift", L"加入时空");
                    row.mid = T(L"paste an invite link or type a rift code",
                                L"粘贴邀请链接，或直接输入房间码");
                    row.right = T(L"Join", L"加入");
                    row.action = RowAction::kRoomJoinPrompt;
                    row.enabled = true;
                    add_row(row);
                }
                {
                    FreightRow row;
                    row.left = T(L"Open rifts", L"公开时空");
                    row.mid = room_list_.empty()
                                  ? std::wstring(T(L"tap refresh", L"点击刷新"))
                                  : std::to_wstring(room_list_.size()) +
                                        T(L" rooms waiting", L" 个房间等待加入");
                    row.right = T(L"Refresh", L"刷新");
                    row.action = RowAction::kRoomRefresh;
                    row.enabled = true;
                    add_row(row);
                }
                const std::size_t limit = std::min<std::size_t>(room_list_.size(), 4);
                for (std::size_t i = 0; i < limit; ++i) {
                    const RoomListEntry& entry = room_list_[i];
                    FreightRow row;
                    row.left = T(L"Rift ", L"房间 ") + Utf8ToWide(entry.code);
                    row.mid = T(L"host ", L"房主 ") + Utf8ToWide(entry.host_name) + L"   " +
                              std::to_wstring(entry.players) + T(L" players", L" 人");
                    row.right = T(L"Join", L"加入");
                    row.action = RowAction::kRoomJoinCode;
                    row.index = static_cast<int>(i);
                    row.enabled = !entry.started;
                    add_row(row);
                }
            } else {
                {
                    FreightRow row;
                    row.left = T(L"Rift ", L"房间 ") + Utf8ToWide(room_code_);
                    row.mid = T(L"host ", L"房主 ") + Utf8ToWide(room_host_name_) + L"   " +
                              std::to_wstring(room_members_.size()) + T(L" players   ", L" 人   ") +
                              (room_started_ ? T(L"in progress", L"对局中")
                                             : T(L"waiting", L"等待开始"));
                    if (room_started_) {
                        row.mid += T(L"   AUDIT ", L"   审计 ") + FormatCompact(room_audit_) +
                                   L"/100";
                    }
                    row.right = T(L"Refresh", L"刷新");
                    row.action = RowAction::kRoomRefresh;
                    row.enabled = true;
                    add_row(row);
                }
                const std::size_t limit = std::min<std::size_t>(room_members_.size(), 5);
                for (std::size_t i = 0; i < limit; ++i) {
                    const RoomMemberInfo& member = room_members_[i];
                    FreightRow row;
                    row.left = Utf8ToWide(member.name);
                    if (member.me) {
                        row.left += T(L" (me)", L"（我）");
                    }
                    if (member.id == room_host_id_) {
                        row.left += T(L" [host]", L" [房主]");
                    }
                    row.mid = L"\u2605 " + FormatCompact(member.score) + T(L"   fleet ", L"   舰队 ") +
                              std::to_wstring(member.fleet) + T(L"   colonies ", L"   殖民地 ") +
                              std::to_wstring(member.colonies) + T(L"   ST ", L"   ST ") +
                              FormatCompact(member.tokens) +
                              (member.online ? L"" : T(L"   offline", L"   离线"));
                    row.right = member.me ? T(L"you", L"你") : L"";
                    row.enabled = false;
                    add_row(row);
                }
                if (!room_started_) {
                    FreightRow row;
                    row.left = T(L"Start game", L"开始游戏");
                    row.mid = room_host_
                                  ? T(L"all players start as commoners at once",
                                      L"所有人以平民身份同时开始")
                                  : T(L"waiting for the host", L"等待房主开始");
                    row.right = T(L"Start", L"开始");
                    row.action = RowAction::kRoomStart;
                    row.enabled = room_host_;
                    add_row(row);
                }
                {
                    FreightRow row;
                    row.left = T(L"Leave rift", L"离开时空");
                    row.mid = T(L"return to local development; the same ID can rejoin within 30 minutes",
                                L"回到本地发展；30 分钟内用同一 ID 回来可继续原存档");
                    row.right = T(L"Leave", L"离开");
                    row.action = RowAction::kRoomLeave;
                    row.enabled = true;
                    add_row(row);
                }
            }
            break;
        }
        }
        UpdatePopupSize();
    }

    // -- 弹窗尺寸与布局 -----------------------------------------------------

    int PopupRowHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 9);
    }

    int PopupTabHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 10);
    }

    int PopupTitleHeight() const {
        return popup_title_line_height_ + ScaleByDpi(current_dpi_, 10);
    }

    int PopupFooterHeight() const {
        return popup_text_line_height_ + ScaleByDpi(current_dpi_, 8);
    }

    int PopupWidth() const {
        return ScaleByDpi(current_dpi_, freight_mode_ ? 720 : 460);
    }

    std::size_t PopupRowCount() const {
        if (freight_mode_) {
            return freight_rows_.size();
        }
        return std::min<std::size_t>(process_snapshot_.top_processes.size(), 10);
    }

    void UpdatePopupSize() {
        const int pad = ScaleByDpi(current_dpi_, 10);
        const int rows = static_cast<int>(PopupRowCount());
        int height = pad + PopupTitleHeight() + ScaleByDpi(current_dpi_, 4);
        if (freight_mode_) {
            height += PopupTabHeight() + ScaleByDpi(current_dpi_, 4);
        }
        height += rows * PopupRowHeight();
        height += ScaleByDpi(current_dpi_, 4) + PopupFooterHeight() + pad;
        height += ScaleByDpi(current_dpi_, 10);
        hover_popup_size_.cx = PopupWidth();
        hover_popup_size_.cy = std::max(height, ScaleByDpi(current_dpi_, 120));
    }

    FreightLayoutRects ComputeFreightLayout() const {
        FreightLayoutRects layout;
        if (popup_window_ == nullptr || !IsWindow(popup_window_)) {
            return layout;
        }
        RECT client{};
        GetClientRect(popup_window_, &client);

        HDC dc = GetDC(popup_window_);
        HFONT font = popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, font);

        const int pad = ScaleByDpi(current_dpi_, 10);
        const int tab_height = PopupTabHeight();
        int x = client.left + pad;
        const int tab_top = client.top + pad + PopupTitleHeight() + ScaleByDpi(current_dpi_, 4);

        const int tab_count = static_cast<int>(FreightTab::kCount);
        for (int i = 0; i < tab_count; ++i) {
            SIZE size = MeasureTextSize(dc, FreightTabLabel(i));
            RECT rect{x, tab_top, x + static_cast<int>(size.cx) + ScaleByDpi(current_dpi_, 22),
                      tab_top + tab_height};
            layout.tabs.push_back(rect);
            x = rect.right + ScaleByDpi(current_dpi_, 4);
        }

        const int row_height = PopupRowHeight();
        int y = tab_top + tab_height + ScaleByDpi(current_dpi_, 4);
        for (std::size_t i = 0; i < freight_rows_.size(); ++i) {
            RECT rect{client.left + pad, y, client.right - pad, y + row_height};
            layout.rows.push_back(rect);

            RECT a{};
            RECT b{};
            if (freight_rows_[i].has_choice) {
                const int button_width = ScaleByDpi(current_dpi_, 128);
                const int gap = ScaleByDpi(current_dpi_, 6);
                b = RECT{rect.right - button_width, y + ScaleByDpi(current_dpi_, 2), rect.right,
                         y + row_height - ScaleByDpi(current_dpi_, 2)};
                a = RECT{b.left - button_width - gap, b.top, b.left - gap, b.bottom};
            }
            layout.rows_a.push_back(a);
            layout.rows_b.push_back(b);
            y += row_height;
        }

        layout.footer = RECT{client.left + pad, y + ScaleByDpi(current_dpi_, 2),
                             client.right - pad, y + ScaleByDpi(current_dpi_, 2) + PopupFooterHeight()};

        SelectObject(dc, old_font);
        ReleaseDC(popup_window_, dc);
        return layout;
    }

    // -- 绘制：任务栏控件 ---------------------------------------------------

    void DrawWidgetContents(HDC dc, const RECT& client_rect) {
        const bool light_theme = IsLightTaskbarTheme();
        const WidgetPalette palette = GetWidgetPalette(light_theme);

        HBRUSH background_brush = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background_brush);
        DeleteObject(background_brush);
        HBRUSH border_brush = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border_brush);
        DeleteObject(border_brush);

        SetBkMode(dc, TRANSPARENT);
        HFONT active_font = font_ != nullptr ? font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ old_font = SelectObject(dc, active_font);

        const int horizontal_padding = ScaleByDpi(current_dpi_, 8);
        const int vertical_padding = ScaleByDpi(current_dpi_, 4);
        const int line_gap = ScaleByDpi(current_dpi_, 2);
        const int column_gap = ScaleByDpi(current_dpi_, 8);
        const int centered_offset =
            std::max<int>(0, ((client_rect.bottom - client_rect.top) - text_line_height_) / 2);
        const int line1_top =
            has_second_line_ ? client_rect.top + vertical_padding : client_rect.top + centered_offset;

        RECT line1_rect{client_rect.left + horizontal_padding, line1_top,
                        client_rect.right - horizontal_padding, line1_top + text_line_height_};
        RECT line2_rect{client_rect.left + horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ + line_gap,
                        client_rect.right - horizontal_padding,
                        client_rect.top + vertical_padding + text_line_height_ * 2 + line_gap};

        if (!widget_columns_.empty()) {
            // 列宽按样本行测好并缓存（与 RefreshFontAndSize 一致），保证控件宽度稳定。
            std::vector<int> column_widths;
            if (widget_column_widths_.size() == widget_columns_.size()) {
                column_widths = widget_column_widths_;
            } else {
                column_widths = MeasureColumnWidths(dc, widget_columns_);
            }
            int current_x = client_rect.left + horizontal_padding;
            for (std::size_t i = 0; i < widget_columns_.size(); ++i) {
                const auto& column = widget_columns_[i];
                const SIZE top_size = MeasureTextSize(dc, column.top_text);
                const SIZE bottom_size = MeasureTextSize(dc, column.bottom_text);
                const int column_width = std::max<int>(
                    column_widths[i], std::max<int>(static_cast<int>(top_size.cx),
                                                    static_cast<int>(bottom_size.cx)));
                RECT top_rect{current_x, line1_rect.top, current_x + column_width, line1_rect.bottom};
                RECT bottom_rect{current_x, line2_rect.top, current_x + column_width, line2_rect.bottom};

                if (!column.top_text.empty()) {
                    SetTextColor(dc, palette.primary_text);
                    DrawTextW(dc, column.top_text.c_str(), static_cast<int>(column.top_text.size()),
                              &top_rect, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
                }
                if (has_second_line_ && !column.bottom_text.empty()) {
                    SetTextColor(dc, palette.secondary_text);
                    DrawTextW(dc, column.bottom_text.c_str(),
                              static_cast<int>(column.bottom_text.size()), &bottom_rect,
                              DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
                }
                current_x += column_width + column_gap;
            }
        } else {
            SetTextColor(dc, palette.primary_text);
            DrawTextW(dc, widget_line1_.c_str(), static_cast<int>(widget_line1_.size()), &line1_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (has_second_line_) {
                SetTextColor(dc, palette.secondary_text);
                DrawTextW(dc, widget_line2_.c_str(), static_cast<int>(widget_line2_.size()),
                          &line2_rect, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
            }
        }

        SelectObject(dc, old_font);
    }

    void PaintWidget() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(widget_window_, &paint_struct);
        RECT client_rect{};
        GetClientRect(widget_window_, &client_rect);
        DrawWidgetContents(dc, client_rect);
        EndPaint(widget_window_, &paint_struct);
    }

    bool RenderLayeredWidget() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return false;
        }
        RECT window_rect{};
        if (!GetWindowRect(widget_window_, &window_rect)) {
            return false;
        }
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        if (width <= 0 || height <= 0) {
            return false;
        }

        HDC screen_dc = GetDC(nullptr);
        HDC memory_dc = CreateCompatibleDC(screen_dc);

        BITMAPINFO bitmap_info{};
        bitmap_info.bmiHeader.biSize = sizeof(bitmap_info.bmiHeader);
        bitmap_info.bmiHeader.biWidth = width;
        bitmap_info.bmiHeader.biHeight = -height;
        bitmap_info.bmiHeader.biPlanes = 1;
        bitmap_info.bmiHeader.biBitCount = 32;
        bitmap_info.bmiHeader.biCompression = BI_RGB;

        void* bitmap_bits = nullptr;
        HBITMAP bitmap =
            CreateDIBSection(screen_dc, &bitmap_info, DIB_RGB_COLORS, &bitmap_bits, nullptr, 0);
        if (bitmap == nullptr || bitmap_bits == nullptr) {
            if (bitmap != nullptr) DeleteObject(bitmap);
            DeleteDC(memory_dc);
            ReleaseDC(nullptr, screen_dc);
            return false;
        }

        HGDIOBJ old_bitmap = SelectObject(memory_dc, bitmap);
        RECT client_rect{0, 0, width, height};
        DrawWidgetContents(memory_dc, client_rect);

        auto* pixels = static_cast<DWORD*>(bitmap_bits);
        const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
        for (size_t i = 0; i < pixel_count; ++i) {
            pixels[i] |= 0xFF000000u;
        }

        POINT source_point{0, 0};
        POINT destination_point{window_rect.left, window_rect.top};
        HWND parent_window = GetParent(widget_window_);
        if (parent_window != nullptr) {
            ScreenToClient(parent_window, &destination_point);
        }
        SIZE window_size{width, height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL update_ok =
            UpdateLayeredWindow(widget_window_, screen_dc, &destination_point, &window_size,
                                memory_dc, &source_point, 0, &blend, ULW_ALPHA);

        SelectObject(memory_dc, old_bitmap);
        DeleteObject(bitmap);
        DeleteDC(memory_dc);
        ReleaseDC(nullptr, screen_dc);
        return update_ok != FALSE;
    }

    void RequestWidgetRedraw() {
        if (!widget_redraw_pending_ && !is_shutting_down_ && controller_window_) {
            widget_redraw_pending_ =
                PostMessageW(controller_window_, kWidgetRedrawMessage, 0, 0) != FALSE;
        }
    }

    void RedrawWidgetNow() {
        if (widget_window_ == nullptr || !IsWindow(widget_window_)) {
            return;
        }
        if (RenderLayeredWidget()) {
            return;
        }
        RedrawWindow(widget_window_, nullptr, nullptr,
                     RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void RefreshWidgetLayoutAndRedraw() {
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
                ShowWindow(widget_window_, SW_HIDE);
                SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                HidePopup();
                return;
            }
        }
        RequestWidgetRedraw();
        if (popup_visible_) {
            PositionPopup();
            RequestPopupRedraw();
        }
    }

    void UpdateLayout() {
        if (!EnsureWidgetWindow()) {
            return;
        }
        if (!embedder_.IsAttached()) {
            ReattachWidget();
            return;
        }
        const UINT previous_dpi = current_dpi_;
        if (embedder_.CurrentDpi() != current_dpi_) {
            RefreshFontAndSize();
        }
        if (current_dpi_ != previous_dpi) {
            RequestWidgetRedraw();
            if (popup_visible_) {
                PositionPopup();
                RequestPopupRedraw();
            }
        }
        if (!embedder_.RefreshLayout(widget_window_, widget_size_)) {
            ShowWindow(widget_window_, SW_HIDE);
            SetTimer(controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
            HidePopup();
            return;
        }
        if (popup_visible_) {
            PositionPopup();
        }
    }

    // -- 绘制：弹窗 ---------------------------------------------------------

    void PaintPopup() {
        PAINTSTRUCT paint_struct{};
        HDC dc = BeginPaint(popup_window_, &paint_struct);
        RECT client_rect{};
        GetClientRect(popup_window_, &client_rect);
        HDC buffer_dc = CreateCompatibleDC(dc);
        HBITMAP buffer = CreateCompatibleBitmap(dc, client_rect.right, client_rect.bottom);
        if (buffer_dc && buffer) {
            HGDIOBJ previous = SelectObject(buffer_dc, buffer);
            DrawPopupContents(buffer_dc, client_rect);
            BitBlt(dc, 0, 0, client_rect.right, client_rect.bottom, buffer_dc, 0, 0, SRCCOPY);
            SelectObject(buffer_dc, previous);
        } else {
            DrawPopupContents(dc, client_rect);
        }
        if (buffer) DeleteObject(buffer);
        if (buffer_dc) DeleteDC(buffer_dc);
        EndPaint(popup_window_, &paint_struct);
    }

    void DrawPopupContents(HDC dc, const RECT& client_rect) {
        const PopupPalette palette = GetPopupPalette(IsLightTaskbarTheme());
        const int pad = ScaleByDpi(current_dpi_, 10);

        HBRUSH background = CreateSolidBrush(palette.background);
        FillRect(dc, &client_rect, background);
        DeleteObject(background);
        HBRUSH border = CreateSolidBrush(palette.border);
        FrameRect(dc, &client_rect, border);
        DeleteObject(border);

        SetBkMode(dc, TRANSPARENT);

        if (freight_mode_) {
            DrawFreightPopup(dc, client_rect, palette, pad);
        } else {
            DrawProcessPopup(dc, client_rect, palette, pad);
        }
    }

    void DrawProcessPopup(HDC dc, const RECT& client_rect, const PopupPalette& palette, int pad) {
        HFONT title_font =
            popup_title_font_ != nullptr ? popup_title_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT body_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        HGDIOBJ old_font = SelectObject(dc, title_font);
        RECT title_rect{client_rect.left + pad, client_rect.top + pad, client_rect.right - pad,
                        client_rect.top + pad + popup_title_line_height_};
        SetTextColor(dc, palette.title_text);
        const std::wstring title = T(L"Process Monitor", L"进程监视器");
        DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &title_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

        const std::wstring stat = FormatW(L"%d proc  %llu%% RAM", process_snapshot_.total_process_count,
                                          static_cast<unsigned long long>(
                                              last_snapshot_.memory_percent < 0 ? 0 : last_snapshot_.memory_percent));
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc, stat.c_str(), static_cast<int>(stat.size()), &title_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);

        SelectObject(dc, body_font);
        const int row_height = PopupRowHeight();
        int y = client_rect.top + pad + PopupTitleHeight() + ScaleByDpi(current_dpi_, 4);

        RECT header{client_rect.left + pad, y, client_rect.right - pad, y + row_height};
        HBRUSH header_brush = CreateSolidBrush(palette.header_fill);
        FillRect(dc, &header, header_brush);
        DeleteObject(header_brush);

        RECT name_rect = header;
        name_rect.left += ScaleByDpi(current_dpi_, 6);
        name_rect.right = header.right - ScaleByDpi(current_dpi_, 160);
        SetTextColor(dc, palette.secondary_text);
        const std::wstring header_text = T(L"NAME", L"进程");
        DrawTextW(dc, header_text.c_str(), static_cast<int>(header_text.size()), &name_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_VCENTER);

        RECT cpu_rect = header;
        cpu_rect.left = header.right - ScaleByDpi(current_dpi_, 160);
        cpu_rect.right = header.right - ScaleByDpi(current_dpi_, 90);
        const std::wstring cpu_text = T(L"CPU", L"CPU");
        DrawTextW(dc, cpu_text.c_str(), static_cast<int>(cpu_text.size()), &cpu_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_VCENTER);

        RECT mem_rect = header;
        mem_rect.left = header.right - ScaleByDpi(current_dpi_, 90);
        mem_rect.right = header.right - ScaleByDpi(current_dpi_, 6);
        const std::wstring mem_text = T(L"MEM", L"内存");
        DrawTextW(dc, mem_text.c_str(), static_cast<int>(mem_text.size()), &mem_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_VCENTER);

        y += row_height;
        const std::size_t row_count = PopupRowCount();
        for (std::size_t i = 0; i < row_count; ++i) {
            const auto& item = process_snapshot_.top_processes[i];
            RECT row{client_rect.left + pad, y, client_rect.right - pad, y + row_height};

            RECT row_name = row;
            row_name.left += ScaleByDpi(current_dpi_, 6);
            row_name.right = row.right - ScaleByDpi(current_dpi_, 160);
            SetTextColor(dc, palette.primary_text);
            const std::wstring name = FormatW(L"%02zu  ", i + 1) + item.name;
            DrawTextW(dc, name.c_str(), static_cast<int>(name.size()), &row_name,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);

            RECT row_cpu = row;
            row_cpu.left = row.right - ScaleDpiOrZero(160);
            row_cpu.right = row.right - ScaleDpiOrZero(90);
            SetTextColor(dc, palette.secondary_text);
            const std::wstring cpu = FormatPercent(item.cpu_percent);
            DrawTextW(dc, cpu.c_str(), static_cast<int>(cpu.size()), &row_cpu,
                      DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_VCENTER);

            RECT row_mem = row;
            row_mem.left = row.right - ScaleDpiOrZero(90);
            row_mem.right = row.right - ScaleDpiOrZero(6);
            const std::wstring mem = FormatBytes(item.rss_bytes);
            DrawTextW(dc, mem.c_str(), static_cast<int>(mem.size()), &row_mem,
                      DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_VCENTER);

            y += row_height;
        }

        SelectObject(dc, old_font);
    }

    int ScaleDpiOrZero(int value) const {
        return ScaleByDpi(current_dpi_, value);
    }

    void DrawFreightPopup(HDC dc, const RECT& client_rect, const PopupPalette& palette, int pad) {
        HFONT title_font =
            popup_title_font_ != nullptr ? popup_title_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT body_font =
            popup_font_ != nullptr ? popup_font_ : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        const FreightLayoutRects layout = ComputeFreightLayout();

        // 标题
        HGDIOBJ old_font = SelectObject(dc, title_font);
        RECT title_rect{client_rect.left + pad, client_rect.top + pad, client_rect.right - pad,
                        client_rect.top + pad + popup_title_line_height_};
        SetTextColor(dc, palette.title_text);
        std::wstring title = T(L"Stellar Lord", L"星际领主");
        DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &title_rect,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);

        std::wstring meta;
        if (!action_report_.empty() && GetTickCount64() < action_report_until_ms_) {
            meta = action_report_;
        } else if (room_active_) {
            meta = T(L"RIFT ", L"时空 ") + Utf8ToWide(room_code_) + L"   " + GetClockText();
        } else {
            meta = T(L"DAY ", L"第 ") + std::to_wstring(game_.day) +
                   T(L"   ", L" 天   ") + GetClockText();
        }
        SetTextColor(dc, palette.secondary_text);
        DrawTextW(dc, meta.c_str(), static_cast<int>(meta.size()), &title_rect,
                  DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS);

        // 标签页
        SelectObject(dc, body_font);
        const int tab_count = static_cast<int>(FreightTab::kCount);
        for (std::size_t i = 0; i < layout.tabs.size() && static_cast<int>(i) < tab_count; ++i) {
            const bool active = static_cast<int>(i) == static_cast<int>(freight_tab_);
            if (active) {
                HBRUSH brush = CreateSolidBrush(palette.tab_active_fill);
                FillRect(dc, &layout.tabs[i], brush);
                DeleteObject(brush);
            }
            RECT text_rect = layout.tabs[i];
            text_rect.left += ScaleByDpi(current_dpi_, 11);
            SetTextColor(dc, active ? palette.accent : palette.secondary_text);
            std::wstring label = FreightTabLabel(static_cast<int>(i));
            if (i == static_cast<std::size_t>(FreightTab::kOnline) && room_active_) {
                label += L" \u25CF";
            }
            DrawTextW(dc, label.c_str(), static_cast<int>(label.size()), &text_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_VCENTER);
        }

        // 行
        for (std::size_t i = 0; i < freight_rows_.size() && i < layout.rows.size(); ++i) {
            const FreightRow& row = freight_rows_[i];
            RECT rect = layout.rows[i];
            const bool hovered = static_cast<int>(i) == popup_hover_row_;
            const bool selected =
                static_cast<int>(i) == freight_selected_ && popup_keyboard_active_;
            if (hovered || selected) {
                HBRUSH brush = CreateSolidBrush(hovered ? palette.tab_active_fill
                                                        : palette.row_highlight);
                FillRect(dc, &rect, brush);
                DeleteObject(brush);
            }

            RECT left_rect = rect;
            left_rect.left += ScaleByDpi(current_dpi_, 6);
            left_rect.right = left_rect.left + ScaleByDpi(current_dpi_, 150);
            SetTextColor(dc, row.enabled ? palette.primary_text : palette.disabled_text);
            DrawTextW(dc, row.left.c_str(), static_cast<int>(row.left.size()), &left_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);

            const int right_reserve = ScaleByDpi(current_dpi_, row.has_choice ? 280 : 150);
            RECT mid_rect = rect;
            mid_rect.left = left_rect.right + ScaleByDpi(current_dpi_, 6);
            mid_rect.right = rect.right - right_reserve;
            SetTextColor(dc, palette.secondary_text);
            DrawTextW(dc, row.mid.c_str(), static_cast<int>(row.mid.size()), &mid_rect,
                      DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);

            if (row.has_choice) {
                const RECT a = layout.rows_a[i];
                const RECT b = layout.rows_b[i];
                auto draw_button = [&](const RECT& button_rect, const std::wstring& text, bool enabled) {
                    HBRUSH brush = CreateSolidBrush(palette.header_fill);
                    FillRect(dc, &button_rect, brush);
                    DeleteObject(brush);
                    HBRUSH frame = CreateSolidBrush(palette.border);
                    FrameRect(dc, &button_rect, frame);
                    DeleteObject(frame);
                    SetTextColor(dc, enabled ? palette.primary_text : palette.disabled_text);
                    RECT text_r = button_rect;
                    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_r,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                };
                draw_button(a, row.choice_a, row.choice_a_enabled);
                draw_button(b, row.choice_b, row.choice_b_enabled);
            } else {
                RECT right_rect = rect;
                right_rect.left = rect.right - ScaleByDpi(current_dpi_, 146);
                right_rect.right -= ScaleByDpi(current_dpi_, 6);
                // 可执行的行把操作列画成一个按钮，否则玩家看不出这里能点。
                if (row.enabled && row.action != RowAction::kNone) {
                    RECT chip = right_rect;
                    chip.top += ScaleByDpi(current_dpi_, 4);
                    chip.bottom -= ScaleByDpi(current_dpi_, 4);
                    HBRUSH chip_fill = CreateSolidBrush(
                        hovered ? palette.row_highlight : palette.header_fill);
                    FillRect(dc, &chip, chip_fill);
                    DeleteObject(chip_fill);
                    HBRUSH chip_frame = CreateSolidBrush(palette.border);
                    FrameRect(dc, &chip, chip_frame);
                    DeleteObject(chip_frame);
                    SetTextColor(dc, palette.accent);
                    DrawTextW(dc, row.right.c_str(), static_cast<int>(row.right.size()), &chip,
                              DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX |
                                  DT_END_ELLIPSIS);
                } else {
                    SetTextColor(dc, palette.disabled_text);
                    DrawTextW(dc, row.right.c_str(), static_cast<int>(row.right.size()),
                              &right_rect,
                              DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX | DT_END_ELLIPSIS |
                                  DT_VCENTER);
                }
            }
        }

        // 底部状态栏
        HBRUSH footer_brush = CreateSolidBrush(palette.header_fill);
        FillRect(dc, &layout.footer, footer_brush);
        DeleteObject(footer_brush);

        RECT footer_text = layout.footer;
        footer_text.left += ScaleByDpi(current_dpi_, 6);
        footer_text.right -= ScaleByDpi(current_dpi_, 6);
        SetTextColor(dc, palette.secondary_text);
        const std::wstring footer =
            L"PWR " + FormatCompact(game_.energy) + L"  MIN " + FormatCompact(game_.minerals) +
            L"  FOOD " + FormatCompact(game_.food) + L"  ALY " + FormatCompact(game_.alloy) +
            L"  POP " + FormatCompact(game_.population) + L"  FLT " +
            FormatCompact(static_cast<double>(game_.fleet)) + L"  MOR " +
            FormatCompact(game_.morale) + L"  ST " + FormatCompact(game_.tokens) +
            L"  INF " + FormatCompact(game_.influence) + L"  REL " +
            std::to_wstring(game_.relics) + L"  COR " + std::to_wstring(game_.cores) +
            L"  DATA " + std::to_wstring(game_.data_cores) + L"  \u2605 " +
            FormatCompact(stellar::Score(game_)) + L"  EVT " +
            std::to_wstring(game_.events.size());
        DrawTextW(dc, footer.c_str(), static_cast<int>(footer.size()), &footer_text,
                  DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);

        SelectObject(dc, old_font);
    }

    void RequestPopupRedraw() {
        if (popup_window_ == nullptr || !IsWindow(popup_window_) || !popup_visible_) {
            return;
        }
        InvalidateRect(popup_window_, nullptr, FALSE);
    }

    // -- 弹窗显隐与定位 -----------------------------------------------------

    bool IsCursorOverWidgetOrPopup() const {
        POINT cursor_pos{};
        GetCursorPos(&cursor_pos);
        return IsPointInWindowRect(widget_window_, cursor_pos) ||
               (popup_visible_ && IsPointInWindowRect(popup_window_, cursor_pos));
    }

    void PositionPopup() {
        if (popup_window_ == nullptr || !IsWindow(popup_window_) || widget_window_ == nullptr ||
            !IsWindow(widget_window_)) {
            return;
        }
        RECT widget_rect{};
        if (!GetWindowRect(widget_window_, &widget_rect)) {
            return;
        }
        HMONITOR monitor_handle = MonitorFromRect(&widget_rect, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitor_info{};
        monitor_info.cbSize = sizeof(monitor_info);
        if (!GetMonitorInfoW(monitor_handle, &monitor_info)) {
            return;
        }

        const RECT bounds = monitor_info.rcMonitor;
        const int gap = ScaleByDpi(current_dpi_, 8);
        const int min_x = static_cast<int>(bounds.left) + gap;
        const int max_x =
            std::max<int>(min_x, static_cast<int>(bounds.right) - hover_popup_size_.cx - gap);
        const int min_y = static_cast<int>(bounds.top) + gap;
        const int max_y =
            std::max<int>(min_y, static_cast<int>(bounds.bottom) - hover_popup_size_.cy - gap);

        const int space_above = static_cast<int>(widget_rect.top - bounds.top);
        const int space_below = static_cast<int>(bounds.bottom - widget_rect.bottom);
        const int space_left = static_cast<int>(widget_rect.left - bounds.left);
        const int space_right = static_cast<int>(bounds.right - widget_rect.right);

        int x = std::clamp<int>(static_cast<int>(widget_rect.right) - hover_popup_size_.cx, min_x, max_x);
        int y = 0;
        const bool prefer_vertical =
            std::max(space_above, space_below) >= std::max(space_left, space_right);
        if (prefer_vertical) {
            if (space_above >= hover_popup_size_.cy + gap || space_above >= space_below) {
                y = widget_rect.top - hover_popup_size_.cy - gap;
            } else {
                y = widget_rect.bottom + gap;
            }
            y = std::clamp<int>(y, min_y, max_y);
        } else {
            if (space_left >= hover_popup_size_.cx + gap || space_left >= space_right) {
                x = static_cast<int>(widget_rect.left) - hover_popup_size_.cx - gap;
            } else {
                x = static_cast<int>(widget_rect.right) + gap;
            }
            x = std::clamp<int>(x, min_x, max_x);
            y = std::clamp<int>(static_cast<int>(widget_rect.bottom) - hover_popup_size_.cy, min_y, max_y);
        }

        SetWindowPos(popup_window_, HWND_TOPMOST, x, y, hover_popup_size_.cx, hover_popup_size_.cy,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void ShowPopup(bool activate) {
        if (!EnsurePopupWindow()) {
            return;
        }
        KillTimer(controller_window_, kHoverHideTimerId);
        if (freight_mode_) {
            RefreshFreightRows();
            freight_selected_ = -1;
            popup_keyboard_active_ = false;
            popup_hover_row_ = -1;
        } else {
            system_worker_.Submit(L"processes", true);
            UpdatePopupSize();
        }
        PositionPopup();
        ShowWindow(popup_window_, activate ? SW_SHOW : SW_SHOWNOACTIVATE);
        popup_visible_ = true;
        RequestPopupRedraw();
        if (activate) {
            SetForegroundWindow(popup_window_);
            SetActiveWindow(popup_window_);
            SetFocus(popup_window_);
        }
    }

    void HidePopup() {
        KillTimer(controller_window_, kHoverHideTimerId);
        popup_hover_row_ = -1;
        popup_visible_ = false;
        popup_keyboard_active_ = false;
        if (popup_window_ != nullptr && IsWindow(popup_window_)) {
            ShowWindow(popup_window_, SW_HIDE);
        }
    }

    void ArmHoverHideTimer() {
        if (CurrentPopupMode() != PopupActivationMode::kHover) {
            return;
        }
        if (controller_window_ == nullptr || !IsWindow(controller_window_)) {
            return;
        }
        SetTimer(controller_window_, kHoverHideTimerId, kHoverHideDelayMs, nullptr);
    }

    void HandleHoverMove(HWND source_window) {
        if (CurrentPopupMode() != PopupActivationMode::kHover) {
            return;
        }
        TRACKMOUSEEVENT track_event{};
        track_event.cbSize = sizeof(track_event);
        track_event.dwFlags = TME_LEAVE;
        track_event.hwndTrack = source_window;
        TrackMouseEvent(&track_event);
        KillTimer(controller_window_, kHoverHideTimerId);
        if (!popup_visible_) {
            ShowPopup(false);
        }
    }

    void HandlePopupMouseMove(HWND source_window, POINT point) {
        HandleHoverMove(source_window);
        TRACKMOUSEEVENT track_event{};
        track_event.cbSize = sizeof(track_event);
        track_event.dwFlags = TME_LEAVE;
        track_event.hwndTrack = source_window;
        TrackMouseEvent(&track_event);
        const int hit = HitTestPopupItem(point);
        const int hover_row = hit >= 0 ? hit : -1;
        if (hover_row != popup_hover_row_) {
            popup_hover_row_ = hover_row;
            RequestPopupRedraw();
        }
    }

    PopupActivationMode CurrentPopupMode() const {
        return freight_mode_ ? app_config_.lord_popup_mode : app_config_.monitor_popup_mode;
    }

    // 弹窗内的命中测试：-2 = 标签页，>=0 = 可点击的行，-1 = 空白或不可点。
    int HitTestPopupItem(POINT point) const {
        if (!freight_mode_) {
            return -1;
        }
        const FreightLayoutRects layout = ComputeFreightLayout();
        for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
            if (PtInRect(&layout.tabs[i], point)) {
                return -2;
            }
        }
        for (std::size_t i = 0; i < layout.rows.size() && i < freight_rows_.size(); ++i) {
            const FreightRow& row = freight_rows_[i];
            if (row.has_choice) {
                if ((row.choice_a_enabled && PtInRect(&layout.rows_a[i], point)) ||
                    (row.choice_b_enabled && PtInRect(&layout.rows_b[i], point))) {
                    return static_cast<int>(i);
                }
            } else if (row.enabled && row.action != RowAction::kNone &&
                       PtInRect(&layout.rows[i], point)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void HandleWidgetLeftButtonDown() {
        click_popup_started_from_widget_ =
            CurrentPopupMode() == PopupActivationMode::kClick && popup_visible_;
    }

    void HandleWidgetLeftButtonUp() {
        if (CurrentPopupMode() != PopupActivationMode::kClick) {
            click_popup_started_from_widget_ = false;
            ShowPopup(true);
            return;
        }
        if (click_popup_started_from_widget_) {
            click_popup_started_from_widget_ = false;
            HidePopup();
            return;
        }
        if (popup_visible_) {
            HidePopup();
            return;
        }
        ShowPopup(true);
    }

    // -- 交互 ---------------------------------------------------------------

    void HandlePopupClick(POINT point) {
        if (!freight_mode_) {
            return;
        }
        popup_keyboard_active_ = true;
        const FreightLayoutRects layout = ComputeFreightLayout();

        for (std::size_t i = 0; i < layout.tabs.size(); ++i) {
            if (PtInRect(&layout.tabs[i], point)) {
                freight_tab_ = static_cast<FreightTab>(i);
                freight_selected_ = -1;
                RefreshFreightRows();
                PositionPopup();
                RequestPopupRedraw();
                return;
            }
        }

        for (std::size_t i = 0; i < layout.rows.size() && i < freight_rows_.size(); ++i) {
            const FreightRow& row = freight_rows_[i];
            if (row.has_choice) {
                if (row.choice_a_enabled && PtInRect(&layout.rows_a[i], point)) {
                    ApplyRowAction(row, true);
                    return;
                }
                if (row.choice_b_enabled && PtInRect(&layout.rows_b[i], point)) {
                    ApplyRowAction(row, false);
                    return;
                }
            } else if (PtInRect(&layout.rows[i], point)) {
                freight_selected_ = static_cast<int>(i);
                ApplyRowAction(row, true);
                return;
            }
        }
    }

    void ApplyRowAction(const FreightRow& row, bool primary) {
        auto feedback = [this](std::wstring text) {
            action_report_ = std::move(text);
            action_report_until_ms_ = GetTickCount64() + 5000;
        };

        switch (row.action) {
        case RowAction::kCopyPlayerId:
            CopyPlayerId();
            return;
        case RowAction::kCopyInviteLink:
            CopyInviteLink();
            return;
        case RowAction::kEditServer:
            ShowServerWindow();
            return;
        case RowAction::kStartHosting:
            StartHostMode(true);
            return;
        case RowAction::kRoomCreate:
            DoRoomCreate();
            return;
        case RowAction::kRoomJoinPrompt:
            ShowRoomWindow(L"");
            return;
        case RowAction::kRoomJoinCode:
            if (row.index >= 0 && row.index < static_cast<int>(room_list_.size())) {
                DoRoomJoin(room_list_[static_cast<std::size_t>(row.index)].code);
            }
            return;
        case RowAction::kRoomLeave:
            DoRoomLeave();
            return;
        case RowAction::kRoomStart:
            DoRoomStart();
            return;
        case RowAction::kRoomRefresh:
            RefreshRoomList();
            if (room_active_) {
                RequestRoomState();
            }
            return;
        default:
            break;
        }

        // 联机对局：所有改变状态的指令都交给服务端结算。
        if (room_active_) {
            if (!room_started_) {
                SetNetStatus(T(L"the host has not started yet", L"房主还未开始游戏"));
                return;
            }
            switch (row.action) {
            case RowAction::kBuild:
                PostRoomAction("build", std::to_string(row.index));
                return;
            case RowAction::kCapital:
                PostRoomAction("capital", {});
                return;
            case RowAction::kResearchTech:
                PostRoomAction("research", std::to_string(row.index));
                return;
            case RowAction::kBuildFleet:
                PostRoomAction("fleet", std::to_string(row.index));
                return;
            case RowAction::kAttackNeutral:
                PostRoomAction("attack_neutral", {});
                return;
            case RowAction::kAttackSector:
                PostRoomAction("attack_target", std::to_string(row.index));
                return;
            case RowAction::kColonize:
                PostRoomAction("colonize", {});
                return;
            case RowAction::kColonyUpgrade:
                PostRoomAction("colony", std::to_string(row.index));
                return;
            case RowAction::kBuildWonder:
                PostRoomAction("wonder", std::to_string(row.index));
                return;
            case RowAction::kChooseFaction:
                PostRoomAction("faction", std::to_string(row.index));
                return;
            case RowAction::kResolveEvent:
                PostRoomAction("resolve_event",
                               std::to_string(row.index) + (primary ? ":1" : ":0"));
                return;
            case RowAction::kTradeResource:
                PostRoomAction(primary ? "sell" : "buy", std::to_string(row.index));
                return;
            case RowAction::kFulfillContract:
                PostRoomAction("contract", std::to_string(row.index));
                return;
            case RowAction::kBuyBlackMarket:
                PostRoomAction("blackmarket", std::to_string(row.index));
                return;
            case RowAction::kLaunchExpedition:
                PostRoomAction(
                    "expedition",
                    std::to_string(row.index) + (primary ? ":0" : ":1"));
                return;
            case RowAction::kClearExpeditionReports:
                PostRoomAction("clear_reports", {});
                return;
            case RowAction::kChooseCommandCard:
                PostRoomAction("card", std::to_string(row.index));
                return;
            case RowAction::kAuditRelief:
                PostRoomAction("audit_relief", primary ? "0" : "1");
                return;
            case RowAction::kAttackRival:
                if (row.index >= 0 && row.index < static_cast<int>(room_members_.size())) {
                    PostRoomAction(primary ? "raid_player" : "attack_player",
                                   room_members_[static_cast<std::size_t>(row.index)].id);
                }
                return;
            default:
                return;
            }
        }

        bool handled = true;
        std::wstring message;
        switch (row.action) {
        case RowAction::kBuild:
            if (stellar::Build(game_, static_cast<stellar::BuildingId>(row.index))) {
                message = T(L"Facility upgraded.", L"设施已升级。");
            } else {
                message = T(L"Not enough resources.", L"资源不足。");
            }
            break;
        case RowAction::kCapital:
            if (stellar::UpgradeCapital(game_)) {
                message = T(L"Capital upgraded.", L"首都已升级。");
            } else {
                message = T(L"Not enough resources.", L"资源不足。");
            }
            break;
        case RowAction::kResearchTech:
            if (stellar::BuyTech(game_, static_cast<stellar::TechId>(row.index))) {
                message = T(L"Research completed.", L"研究完成。");
            } else {
                message = T(L"Not enough research points.", L"科研点不足。");
            }
            break;
        case RowAction::kBuildFleet:
            if (stellar::BuildFleet(game_, row.index)) {
                message = T(L"Squadron built.", L"舰队已建造。");
            } else {
                message = T(L"Need more population / alloy / berths.",
                            L"人口、合金或舰队容量不足。");
            }
            break;
        case RowAction::kAttackNeutral: {
            const stellar::BattleResult result = stellar::AttackNeutral(game_);
            ReportBattle(result);
            break;
        }
        case RowAction::kAttackSector: {
            const auto target = static_cast<stellar::SectorTargetId>(row.index);
            const stellar::BattleResult result = stellar::AttackSector(game_, target);
            ReportBattle(result);
            break;
        }
        case RowAction::kColonize:
            if (stellar::Colonize(game_)) {
                message = T(L"Colony founded.", L"新殖民地已建立。");
            } else {
                message = T(L"Need 8 population and enough supplies.",
                            L"需要 8 人口和足够的补给。");
            }
            break;
        case RowAction::kColonyUpgrade:
            if (stellar::UpgradeColony(game_, static_cast<std::size_t>(row.index))) {
                message = T(L"Colony upgraded.", L"殖民地已升级。");
            } else {
                message = T(L"Not enough resources.", L"资源不足。");
            }
            break;
        case RowAction::kBuildWonder:
            if (stellar::BuildWonder(game_, row.index)) {
                message = std::wstring(T(L"Wonder completed: ", L"奇观建成：")) +
                          WonderName(row.index);
            } else {
                message = T(L"Not enough resources.", L"资源不足。");
            }
            break;
        case RowAction::kChooseFaction:
            if (stellar::ChooseFaction(game_, row.index)) {
                message = std::wstring(T(L"Doctrine locked: ", L"治国路线已锁定：")) +
                          FactionName(row.index);
            } else {
                message = T(L"Already chosen.", L"已经选择过了。");
            }
            break;
        case RowAction::kResolveEvent:
            if (row.index >= 0 &&
                row.index < static_cast<int>(game_.events.size())) {
                const int event_id = game_.events[static_cast<std::size_t>(row.index)].id;
                const std::wstring title = GetEventDisplay(event_id).title;
                if (stellar::ResolveEvent(game_, static_cast<std::size_t>(row.index),
                                          primary)) {
                    message = std::wstring(T(L"Event resolved: ", L"事件已处理：")) + title;
                }
            }
            break;
        case RowAction::kTradeResource:
            if (primary) {
                if (stellar::SellResource(game_, row.index)) {
                    message = std::wstring(T(L"Sold 50 ", L"卖出 50 ")) +
                              ResourceName(row.index) +
                              T(L" for ST.", L"，换成了星际代币。");
                } else {
                    message = T(L"Not enough goods to sell.",
                                L"库存不足，卖不了这么多。");
                }
            } else {
                if (stellar::BuyResource(game_, row.index)) {
                    message = std::wstring(T(L"Bought 50 ", L"买入 50 ")) +
                              ResourceName(row.index) + T(L".", L"。");
                } else {
                    message = T(L"Not enough ST.", L"星际代币不足。");
                }
            }
            break;
        case RowAction::kFulfillContract:
            if (stellar::FulfillContract(game_, static_cast<std::size_t>(row.index))) {
                message = T(L"Contract delivered: ST and influence received.",
                            L"合同已交付：获得星际代币和影响力。");
            } else {
                message = T(L"Not enough goods, or the contract is already done.",
                            L"货物不足，或合同已经完成。");
            }
            break;
        case RowAction::kBuyBlackMarket:
            if (stellar::BuyBlackMarket(game_, static_cast<std::size_t>(row.index))) {
                message = T(L"Black market purchase complete.",
                            L"黑市交易完成。");
            } else {
                message = T(L"Not enough ST, or the lot is sold out.",
                            L"星际代币不足，或商品已售罄。");
            }
            break;
        case RowAction::kLaunchExpedition: {
            const auto destination =
                static_cast<stellar::ExpeditionDestination>(row.index);
            const auto stance = primary ? stellar::ExpeditionStance::kCautious
                                        : stellar::ExpeditionStance::kDeep;
            if (stellar::LaunchExpedition(game_, destination, stance)) {
                message = std::wstring(T(L"Expedition launched: ", L"远征队已出发：")) +
                          ExpeditionName(row.index) + L" · " + ExpeditionStanceName(
                              static_cast<int>(stance));
            } else {
                message = T(L"Not enough ships, or all expedition slots are full.",
                            L"舰队不足，或远征队名额已满。");
            }
            break;
        }
        case RowAction::kClearExpeditionReports:
            stellar::ClearExpeditionReports(game_);
            message = T(L"Expedition reports cleared.", L"返航报告已清除。");
            break;
        case RowAction::kChooseCommandCard: {
            const int card = stellar::CommandCardOffer(game_, row.index);
            if (stellar::ChooseCommandCard(game_, row.index)) {
                message = std::wstring(T(L"Command card taken: ", L"指令卡已使用：")) +
                          CommandCardName(card);
            } else {
                message = T(L"Command card expired or cannot be paid.",
                            L"指令卡已过期，或代价付不起。");
            }
            break;
        }
        case RowAction::kAuditRelief:
            message = T(L"Audit pressure can only be relieved inside a rift.",
                        L"审计压力只能在联机时空中缓解。");
            break;
        default:
            handled = false;
            break;
        }
        if (!handled) {
            return;
        }
        if (!message.empty()) {
            feedback(std::move(message));
        }

        UpdateFreightDisplayLines();
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            embedder_.RefreshLayout(widget_window_, widget_size_);
        }
        RequestWidgetRedraw();
        RefreshFreightRows();
        PositionPopup();
        RequestPopupRedraw();
    }

    void ReportBattle(const stellar::BattleResult& result) {
        std::wstring text;
        if (result.won) {
            text = T(L"Victory", L"胜利");
            if (result.captured) {
                text += T(L"  ·  colony captured", L"  ·  夺取殖民地");
            }
            text += T(L"  ·  loot MIN ", L"  ·  掠夺矿物 ") + FormatCompact(result.loot_minerals);
            text += L" ALY " + FormatCompact(result.loot_alloy);
            if (result.loot_research > 0.5) {
                text += T(L" SCI ", L" 科研 ") + FormatCompact(result.loot_research);
            }
            if (result.loot_tokens > 0.5) {
                text += T(L" ST ", L" 星际代币 ") + FormatCompact(result.loot_tokens);
            }
        } else {
            text = T(L"Defeat  ·  fleet lost ", L"战败  ·  损失舰队 ") +
                   std::to_wstring(result.attacker_loss);
        }
        action_report_ = text;
        action_report_until_ms_ = GetTickCount64() + 8000;
    }

    void HandlePopupKey(WPARAM key) {
        if (!freight_mode_) {
            if (key == VK_ESCAPE) {
                HidePopup();
            }
            return;
        }
        popup_keyboard_active_ = true;
        switch (key) {
        case VK_ESCAPE:
            HidePopup();
            break;
        case VK_LEFT:
            SwitchTab(-1);
            break;
        case VK_RIGHT:
            SwitchTab(1);
            break;
        case VK_UP:
            MoveSelection(-1);
            break;
        case VK_DOWN:
            MoveSelection(1);
            break;
        case VK_RETURN:
        case VK_SPACE:
            if (freight_selected_ >= 0 &&
                freight_selected_ < static_cast<int>(freight_rows_.size())) {
                ApplyRowAction(freight_rows_[static_cast<std::size_t>(freight_selected_)], true);
            }
            break;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
            freight_tab_ = static_cast<FreightTab>(key - '1');
            freight_selected_ = -1;
            RefreshFreightRows();
            PositionPopup();
            RequestPopupRedraw();
            break;
        case 'Q':
        case 'A':
            if (freight_selected_ >= 0 &&
                freight_selected_ < static_cast<int>(freight_rows_.size())) {
                ApplyRowAction(freight_rows_[static_cast<std::size_t>(freight_selected_)], true);
            }
            break;
        case 'E':
        case 'B':
            if (freight_selected_ >= 0 &&
                freight_selected_ < static_cast<int>(freight_rows_.size())) {
                ApplyRowAction(freight_rows_[static_cast<std::size_t>(freight_selected_)], false);
            }
            break;
        default:
            break;
        }
    }

    void SwitchTab(int delta) {
        int tab = static_cast<int>(freight_tab_) + delta;
        const int count = static_cast<int>(FreightTab::kCount);
        tab = ((tab % count) + count) % count;
        freight_tab_ = static_cast<FreightTab>(tab);
        freight_selected_ = -1;
        RefreshFreightRows();
        PositionPopup();
        RequestPopupRedraw();
    }

    void MoveSelection(int delta) {
        if (freight_rows_.empty()) {
            return;
        }
        int index = freight_selected_ + delta;
        freight_selected_ = std::clamp(index, 0, static_cast<int>(freight_rows_.size()) - 1);
        RequestPopupRedraw();
    }

    // -- 联机 ---------------------------------------------------------------

    void RefreshOnlineUi() {
        if (popup_visible_ && freight_mode_) {
            RefreshFreightRows();
            PositionPopup();
            RequestPopupRedraw();
        }
    }

    void SetNetStatus(const std::wstring& text) {
        net_status_ = text;
        RefreshOnlineUi();
    }

    // 每个请求一个后台线程；只捕获值，不碰 this，所以窗口销毁也不会悬空。
    void PostNetTask(NetTaskKind kind, std::string path, std::string body, bool is_post,
                     std::string tag = {}) {
        if (is_shutting_down_ || controller_window_ == nullptr) {
            return;
        }
        const HWND target = controller_window_;
        const std::string host =
            app_config_.server_host.empty() ? std::string("127.0.0.1") : app_config_.server_host;
        const unsigned short port = static_cast<unsigned short>(app_config_.server_port);
        const std::string version =
            std::string("v=") + std::to_string(rift_server::kProtocolVersion);
        if (is_post) {
            if (!body.empty()) {
                body += "&";
            }
            body += version;
        } else {
            path += path.find('?') == std::string::npos ? "?" : "&";
            path += version;
        }
        std::thread([kind, target, host, port, path = std::move(path), body = std::move(body),
                     is_post, tag = std::move(tag)]() {
            NetResponse response = is_post ? HttpPostForm(host, port, path, body)
                                           : HttpGet(host, port, path);
            auto* result = new NetTaskResult{kind, std::move(response), tag};
            if (!PostMessageW(target, kNetResultMessage, 0, reinterpret_cast<LPARAM>(result))) {
                delete result;
            }
        }).detach();
    }

    void RequestRoomState() {
        if (!room_active_ || room_poll_pending_) {
            return;
        }
        room_poll_pending_ = true;
        PostNetTask(NetTaskKind::kRoomState,
                    "/api/room/state?id=" + UrlEncode(app_config_.player_id) + "&room=" +
                        UrlEncode(room_code_),
                    {}, false);
    }

    void RefreshRoomList() {
        SetNetStatus(T(L"scanning...", L"正在扫描…"));
        PostNetTask(NetTaskKind::kRoomList, "/api/room/list", {}, false);
    }

    void CopyPlayerId() {
        if (!OpenClipboard(controller_window_)) {
            SetNetStatus(T(L"clipboard busy", L"剪贴板被占用"));
            return;
        }
        EmptyClipboard();
        const std::wstring id = Utf8ToWide(app_config_.player_id);
        const size_t bytes = (id.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool copied = false;
        if (memory != nullptr) {
            void* target = GlobalLock(memory);
            if (target != nullptr) {
                std::memcpy(target, id.c_str(), bytes);
                GlobalUnlock(memory);
                if (SetClipboardData(CF_UNICODETEXT, memory) != nullptr) {
                    copied = true;
                }
            }
            if (!copied) {
                GlobalFree(memory);
            }
        }
        CloseClipboard();
        SetNetStatus(copied ? T(L"ID copied", L"ID 已复制")
                            : T(L"copy failed", L"复制失败"));
    }

    // -- 服务器设置窗口 -------------------------------------------------------

    void ShowServerWindow() {
        if (server_window_ != nullptr && IsWindow(server_window_)) {
            SetWindowTextW(server_host_edit_, Utf8ToWide(app_config_.server_host).c_str());
            SetWindowTextW(server_port_edit_,
                           std::to_wstring(app_config_.server_port).c_str());
            ShowWindow(server_window_, SW_SHOWNORMAL);
            SetForegroundWindow(server_window_);
            return;
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = ScaleByDpi(current_dpi_, kServerWindowWidth);
        const int height = ScaleByDpi(current_dpi_, kServerWindowHeight);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        server_window_ = CreateWindowExW(WS_EX_TOPMOST, kServerClassName,
                                         T(L"Stellar Lord - Server", L"星际领主 · 服务器"),
                                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width,
                                         height, nullptr, nullptr, instance_handle_, this);
        if (server_window_ == nullptr) {
            return;
        }
        ShowWindow(server_window_, SW_SHOWNORMAL);
        SetForegroundWindow(server_window_);
    }

    void HideServerWindow() {
        if (server_window_ != nullptr && IsWindow(server_window_)) {
            ShowWindow(server_window_, SW_HIDE);
        }
    }

    void CreateServerChildren(HWND window_handle) {
        RECT client{};
        GetClientRect(window_handle, &client);
        HFONT font = popup_font_ != nullptr ? popup_font_
                                            : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int pad = ScaleByDpi(current_dpi_, 14);
        const int label_width = ScaleByDpi(current_dpi_, 78);
        const int row_height = ScaleByDpi(current_dpi_, 24);
        const int gap = ScaleByDpi(current_dpi_, 10);
        const int button_width = ScaleByDpi(current_dpi_, 88);
        const int button_height = ScaleByDpi(current_dpi_, 28);
        const int inner_width = (client.right - client.left) - pad * 2;

        auto make_label = [&](const wchar_t* text, int x, int y, int w, int h) {
            HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x,
                                         y, w, h, window_handle, nullptr, instance_handle_,
                                         nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return label;
        };

        int y = pad;
        make_label(T(L"Host / IP", L"主机 / IP"), pad, y, label_width, row_height);
        server_host_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(app_config_.server_host).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP, pad + label_width, y,
            inner_width - label_width, row_height, window_handle, nullptr, instance_handle_,
            nullptr);

        y += row_height + gap;
        make_label(T(L"Port", L"端口"), pad, y, label_width, row_height);
        server_port_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", std::to_wstring(app_config_.server_port).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER | WS_TABSTOP, pad + label_width, y,
            inner_width - label_width, row_height, window_handle, nullptr, instance_handle_,
            nullptr);

        y += row_height + gap;
        const int button_y = (client.bottom - client.top) - pad - button_height;
        server_status_text_ = make_label(
            T(L"Point the client at the stellar_lord_server address.",
              L"填写 stellar_lord_server 所在机器地址，本机可留 127.0.0.1。"),
            pad, y, inner_width, std::max(row_height, button_y - y - gap));

        CreateWindowExW(0, L"BUTTON", T(L"Save", L"保存"),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width * 2 - gap, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kServerSaveCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Cancel", L"取消"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width, button_y, button_width,
                        button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kServerCancelCommandId)),
                        instance_handle_, nullptr);

        SendMessageW(server_host_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(server_port_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(server_window_, DM_SETDEFID, kServerSaveCommandId, 0);
    }

    void SubmitServerConfig() {
        if (server_host_edit_ == nullptr || !IsWindow(server_host_edit_)) {
            return;
        }
        wchar_t host_buffer[256]{};
        wchar_t port_buffer[16]{};
        GetWindowTextW(server_host_edit_, host_buffer, static_cast<int>(std::size(host_buffer)));
        GetWindowTextW(server_port_edit_, port_buffer, static_cast<int>(std::size(port_buffer)));

        std::wstring host = TrimWide(host_buffer);
        // 允许直接粘贴 "http://1.2.3.4/" 这种形式，这里只留主机名。
        const std::wstring lower = host;
        auto starts_with = [&](const wchar_t* prefix) {
            return lower.compare(0, wcslen(prefix), prefix) == 0;
        };
        if (starts_with(L"http://")) {
            host = host.substr(7);
        } else if (starts_with(L"https://")) {
            host = host.substr(8);
        }
        while (!host.empty() && (host.back() == L'/' || host.back() == L' ')) {
            host.pop_back();
        }
        const size_t slash = host.find(L'/');
        if (slash != std::wstring::npos) {
            host = host.substr(0, slash);
        }

        const int port = static_cast<int>(std::wcstol(port_buffer, nullptr, 10));
        if (host.empty() || host.size() > 253) {
            SetServerStatus(T(L"Host cannot be empty.", L"主机地址不能为空。"));
            return;
        }
        for (wchar_t ch : host) {
            if (ch == L' ' || ch == L'\t') {
                SetServerStatus(T(L"Host cannot contain spaces.", L"主机地址不能包含空格。"));
                return;
            }
        }
        if (port < 1 || port > 65535) {
            SetServerStatus(T(L"Port must be 1-65535.", L"端口需在 1-65535 之间。"));
            return;
        }

        app_config_.server_host = WideToUtf8(host);
        app_config_.server_port = static_cast<unsigned int>(port);
        SaveAppConfig(app_config_);

        // 换了服务器，旧服务器上的房间不再适用，回到单机。
        if (room_active_) {
            ClearRoomUi();
            game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
            last_tick_ms_ = GetTickCount64();
        }
        room_list_.clear();
        net_status_ = T(L"server updated", L"服务器已更新");

        HideServerWindow();
        RefreshOnlineUi();
    }

    void SetServerStatus(const std::wstring& text) {
        if (server_status_text_ != nullptr && IsWindow(server_status_text_)) {
            SetWindowTextW(server_status_text_, text.c_str());
        }
    }

    static LRESULT CALLBACK ServerWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                             LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->server_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_CREATE:
            app->CreateServerChildren(window_handle);
            return 0;
        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED) {
                const UINT id = LOWORD(w_param);
                if (id == kServerSaveCommandId) {
                    app->SubmitServerConfig();
                    return 0;
                }
                if (id == kServerCancelCommandId) {
                    app->HideServerWindow();
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            app->HideServerWindow();
            return 0;
        case WM_NCDESTROY:
            if (app->server_window_ == window_handle) {
                app->server_window_ = nullptr;
                app->server_host_edit_ = nullptr;
                app->server_port_edit_ = nullptr;
                app->server_status_text_ = nullptr;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    // -- 启动选择与内嵌主机 ---------------------------------------------------

    void DestroyLaunchWindow() {
        if (launch_window_ != nullptr && IsWindow(launch_window_)) {
            DestroyWindow(launch_window_);
        }
        launch_window_ = nullptr;
    }

    void ShowLaunchWindow() {
        if (launch_window_ != nullptr && IsWindow(launch_window_)) {
            ShowWindow(launch_window_, SW_SHOWNORMAL);
            SetForegroundWindow(launch_window_);
            return;
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = ScaleByDpi(current_dpi_, kLaunchWindowWidth);
        const int height = ScaleByDpi(current_dpi_, kLaunchWindowHeight);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        launch_window_ = CreateWindowExW(WS_EX_TOPMOST, kLaunchClassName,
                                         T(L"Stellar Lord - Start", L"星际领主 · 启动"),
                                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width,
                                         height, nullptr, nullptr, instance_handle_, this);
        if (launch_window_ == nullptr) {
            return;
        }
        ShowWindow(launch_window_, SW_SHOWNORMAL);
        SetForegroundWindow(launch_window_);
    }

    void CreateLaunchChildren(HWND window_handle) {
        RECT client{};
        GetClientRect(window_handle, &client);
        HFONT font = popup_font_ != nullptr ? popup_font_
                                            : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT title_font = popup_title_font_ != nullptr
                               ? popup_title_font_
                               : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int pad = ScaleByDpi(current_dpi_, 18);
        const int gap = ScaleByDpi(current_dpi_, 10);
        const int title_height = ScaleByDpi(current_dpi_, 28);
        const int row_height = ScaleByDpi(current_dpi_, 22);
        const int button_height = ScaleByDpi(current_dpi_, 40);
        const int inner_width = (client.right - client.left) - pad * 2;
        const int button_width = (inner_width - gap) / 2;

        auto make_label = [&](const wchar_t* text, int x, int y, int w, int h, HFONT use_font,
                              UINT style) {
            HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style, x, y,
                                         w, h, window_handle, nullptr, instance_handle_, nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(use_font), TRUE);
            return label;
        };

        int y = pad;
        make_label(T(L"Stellar Lord", L"星际领主"), pad, y, inner_width, title_height,
                   title_font, SS_LEFT);
        y += title_height + ScaleByDpi(current_dpi_, 4);
        make_label(T(L"Choose this PC's role in the rift network.",
                     L"选择这台电脑在时空网络中的角色。"),
                   pad, y, inner_width, row_height, font, SS_LEFT);

        const int caption_height = ScaleByDpi(current_dpi_, 38);
        const int button_y = (client.bottom - client.top) - pad - caption_height -
                             ScaleByDpi(current_dpi_, 4) - button_height;

        CreateWindowExW(0, L"BUTTON", T(L"Start as server", L"作为服务器启动"),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, pad, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kLaunchServerCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Start as client", L"作为客户端启动"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        pad + button_width + gap, button_y, button_width, button_height,
                        window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kLaunchClientCommandId)),
                        instance_handle_, nullptr);

        const int caption_y = button_y + button_height + ScaleByDpi(current_dpi_, 6);
        make_label(T(L"Creates the rift and an invite link.",
                     L"在这台电脑上创建时空，并生成邀请链接。"),
                   pad, caption_y, button_width, caption_height, font, SS_LEFT | SS_NOPREFIX);
        make_label(T(L"Paste a friend's invite link to join.",
                     L"粘贴朋友发来的邀请链接，直接加入。"),
                   pad + button_width + gap, caption_y, button_width, caption_height, font,
                   SS_LEFT | SS_NOPREFIX);
    }

    static LRESULT CALLBACK LaunchWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                            LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->launch_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_CREATE:
            app->CreateLaunchChildren(window_handle);
            return 0;
        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED) {
                const UINT id = LOWORD(w_param);
                if (id == kLaunchServerCommandId) {
                    app->StartHostMode(true);
                    return 0;
                }
                if (id == kLaunchClientCommandId) {
                    app->ShowRoomWindow(L"");
                    app->DestroyLaunchWindow();
                    app->SetFocusToRoomInput();
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            // 用户选择退出：交给主窗口统一收尾。
            if (app->controller_window_ != nullptr) {
                PostMessageW(app->controller_window_, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_NCDESTROY:
            if (app->launch_window_ == window_handle) {
                app->launch_window_ = nullptr;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    std::wstring CurrentInviteLink() const {
        if (!room_active_ || room_code_.empty()) {
            return {};
        }
        std::string host = room_host_ ? host_lan_ip_ : app_config_.server_host;
        if (host.empty()) {
            host = "127.0.0.1";
        }
        unsigned int port = room_host_ ? host_port_ : app_config_.server_port;
        if (port == 0) {
            port = app_config_.server_port != 0 ? app_config_.server_port : 8787;
        }
        return BuildInviteLink(host, port, room_code_);
    }

    void CopyInviteLink() {
        if (!room_active_) {
            net_status_ = T(L"host or join a rift first", L"请先开启主机或加入时空");
            RefreshOnlineUi();
            MessageBoxW(controller_window_,
                        T(L"Open a rift first, then the invite link becomes available.",
                          L"请先开启主机或加入时空，之后才能复制邀请链接。"),
                        T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONINFORMATION);
            return;
        }
        const std::wstring link = CurrentInviteLink();
        const bool copied = WriteClipboardText(controller_window_, link);
        const std::wstring message =
            copied ? T(L"invite link copied", L"邀请链接已复制")
                   : T(L"copy failed", L"复制失败");
        SetNetStatus(message);
        SetHostStatus(message);
        action_report_ = message;
        action_report_until_ms_ = GetTickCount64() + 5000;
    }

    void StartHostMode(bool interactive) {
        if (is_shutting_down_) {
            return;
        }
        if (host_pending_) {
            if (interactive) {
                ShowHostWindow();
            }
            return;
        }
        if (room_active_) {
            if (room_host_) {
                if (interactive) {
                    ShowHostWindow();
                } else {
                    ShowPopup(true);
                }
            } else {
                MessageBoxW(controller_window_,
                            T(L"Leave the joined rift before hosting your own.",
                              L"请先离开当前加入的时空，再开启自己的主机。"),
                            T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONINFORMATION);
            }
            return;
        }
        if (!freight_mode_) {
            ToggleMode();
        }
        if (player_name_.empty()) {
            player_name_ = "Lord";
        }
        const unsigned int port =
            (app_config_.server_port >= 1 && app_config_.server_port <= 65535)
                ? app_config_.server_port
                : 8787u;
        if (rift_server::IsRunning() && rift_server::Port() != port) {
            rift_server::Stop();
        }
        if (!rift_server::IsRunning()) {
            std::string error;
            if (!rift_server::Start(static_cast<unsigned short>(port), &error)) {
                std::wstring reason = T(L"Cannot start the embedded server on port ",
                                        L"无法在本机端口 ") +
                                      std::to_wstring(port) + T(L". ", L" 启动主机。 ");
                if (!error.empty()) {
                    reason += Utf8ToWide(error);
                }
                net_status_ = reason;
                RefreshOnlineUi();
                MessageBoxW(controller_window_, reason.c_str(),
                            T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONWARNING);
                return;
            }
        }

        host_port_ = port;
        host_lan_ip_ = DetectLanIpv4();
        host_pending_ = true;
        pending_host_window_ = interactive;
        app_config_.server_host = "127.0.0.1";
        app_config_.server_port = port;
        SaveAppConfig(app_config_);
        DestroyLaunchWindow();

        freight_tab_ = FreightTab::kOnline;
        freight_selected_ = -1;
        DoRoomCreate("host");
        RefreshFreightRows();
        if (interactive) {
            ShowHostWindow();
        } else {
            ShowPopup(true);
        }
        PositionPopup();
        RequestPopupRedraw();
    }

    void ShowHostWindow() {
        if (host_window_ != nullptr && IsWindow(host_window_)) {
            RefreshHostWindow();
            ShowWindow(host_window_, SW_SHOWNORMAL);
            SetForegroundWindow(host_window_);
            return;
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = ScaleByDpi(current_dpi_, kHostWindowWidth);
        const int height = ScaleByDpi(current_dpi_, kHostWindowHeight);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        host_window_ = CreateWindowExW(WS_EX_TOPMOST, kHostClassName,
                                       T(L"Stellar Lord - Host", L"星际领主 · 主机"),
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width,
                                       height, nullptr, nullptr, instance_handle_, this);
        if (host_window_ == nullptr) {
            return;
        }
        RefreshHostWindow();
        ShowWindow(host_window_, SW_SHOWNORMAL);
        SetForegroundWindow(host_window_);
    }

    void HideHostWindow() {
        if (host_window_ != nullptr && IsWindow(host_window_)) {
            ShowWindow(host_window_, SW_HIDE);
        }
    }

    void SetHostStatus(const std::wstring& text) {
        if (host_status_text_ != nullptr && IsWindow(host_status_text_)) {
            SetWindowTextW(host_status_text_, text.c_str());
        }
    }

    void RefreshHostWindow() {
        if (host_window_ == nullptr || !IsWindow(host_window_)) {
            return;
        }
        if (host_invite_edit_ != nullptr && IsWindow(host_invite_edit_)) {
            SetWindowTextW(host_invite_edit_, CurrentInviteLink().c_str());
        }
        std::wstring status;
        if (host_pending_) {
            status = T(L"Preparing the rift...", L"正在准备时空…");
        } else if (room_active_ && room_host_) {
            status = std::wstring(T(L"Rift ", L"时空 ")) + Utf8ToWide(room_code_) +
                     T(L" is live. Keep this program running so friends can join.",
                       L" 已开启。保持程序运行，好友才能持续加入。");
        } else {
            status = T(L"No active rift.", L"当前没有开启的时空。");
        }
        SetHostStatus(status);
    }

    void CreateHostChildren(HWND window_handle) {
        RECT client{};
        GetClientRect(window_handle, &client);
        HFONT font = popup_font_ != nullptr ? popup_font_
                                            : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT title_font = popup_title_font_ != nullptr
                               ? popup_title_font_
                               : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int pad = ScaleByDpi(current_dpi_, 16);
        const int gap = ScaleByDpi(current_dpi_, 9);
        const int row_height = ScaleByDpi(current_dpi_, 24);
        const int title_height = ScaleByDpi(current_dpi_, 26);
        const int button_width = ScaleByDpi(current_dpi_, 118);
        const int button_height = ScaleByDpi(current_dpi_, 30);
        const int inner_width = (client.right - client.left) - pad * 2;

        auto make_label = [&](const wchar_t* text, int x, int y, int w, int h, HFONT use_font) {
            HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x,
                                         y, w, h, window_handle, nullptr, instance_handle_,
                                         nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(use_font), TRUE);
            return label;
        };

        int y = pad;
        make_label(T(L"Invite link", L"邀请链接"), pad, y, inner_width, title_height, title_font);
        y += title_height + gap;
        host_invite_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, pad, y,
            inner_width, row_height, window_handle, nullptr, instance_handle_, nullptr);
        y += row_height + gap;
        make_label(T(L"Send this link to friends. They paste it in the client to join.",
                     L"把这条链接发给朋友，他们在客户端粘贴即可加入。"),
                   pad, y, inner_width, row_height, font);
        y += row_height + gap;
        const int button_y = (client.bottom - client.top) - pad - button_height;
        host_status_text_ = make_label(T(L"Preparing the rift...", L"正在准备时空…"), pad, y,
                                       inner_width, std::max(row_height, button_y - y - gap),
                                       font);

        CreateWindowExW(0, L"BUTTON", T(L"Copy link", L"复制链接"),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width * 2 - gap, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kHostCopyCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Enter game", L"进入游戏"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width, button_y, button_width,
                        button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kHostEnterCommandId)),
                        instance_handle_, nullptr);

        SendMessageW(host_invite_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(host_window_, DM_SETDEFID, kHostCopyCommandId, 0);
    }

    static LRESULT CALLBACK HostWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                          LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->host_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_CREATE:
            app->CreateHostChildren(window_handle);
            return 0;
        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED) {
                const UINT id = LOWORD(w_param);
                if (id == kHostCopyCommandId) {
                    app->CopyInviteLink();
                    return 0;
                }
                if (id == kHostEnterCommandId) {
                    app->pending_host_window_ = false;
                    app->HideHostWindow();
                    app->freight_tab_ = FreightTab::kOnline;
                    app->RefreshFreightRows();
                    app->ShowPopup(true);
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            app->pending_host_window_ = false;
            app->HideHostWindow();
            return 0;
        case WM_NCDESTROY:
            if (app->host_window_ == window_handle) {
                app->host_window_ = nullptr;
                app->host_invite_edit_ = nullptr;
                app->host_status_text_ = nullptr;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    // -- 时空房间 -------------------------------------------------------------

    std::wstring NetErrorText(const std::string& code) const {
        if (code == "no_room") return T(L"room not found", L"房间不存在");
        if (code == "started") return T(L"game already started", L"对局已经开始");
        if (code == "not_started") return T(L"game not started", L"对局尚未开始");
        if (code == "not_host") return T(L"only the host can start", L"只有房主能开始游戏");
        if (code == "room_full") return T(L"room is full", L"房间人数已满");
        if (code == "bad_room") return T(L"invalid room code", L"房间码无效");
        if (code == "rejected") return T(L"action not allowed now", L"当前条件不满足");
        if (code == "not_found") return T(L"server is outdated", L"服务器版本过旧");
        if (code == "version") {
            return T(L"version mismatch - update both game copies to the same build",
                     L"游戏版本不一致，请把双方的星际领主更新到同一版本");
        }
        return T(L"request failed", L"请求失败");
    }

    std::string PlayerName() const {
        return player_name_;
    }

    void DoRoomCreate(const std::string& tag = {}) {
        SetNetStatus(T(L"creating rift...", L"正在创建时空…"));
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&name=" + UrlEncode(player_name_);
        PostNetTask(NetTaskKind::kRoomCreate, "/api/room/create", form, true, tag);
    }

    void DoRoomJoin(const std::string& code) {
        SetNetStatus(T(L"joining rift...", L"正在加入时空…"));
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&name=" + UrlEncode(player_name_) + "&room=" + UrlEncode(code);
        PostNetTask(NetTaskKind::kRoomJoin, "/api/room/join", form, true);
    }

    void ClearRoomUi() {
        room_active_ = false;
        room_started_ = false;
        room_host_ = false;
        room_code_.clear();
        room_host_id_.clear();
        room_host_name_.clear();
        room_audit_ = 15.0;
        room_audit_tier_ = 0;
        room_members_.clear();
        room_poll_pending_ = false;
        room_list_.clear();
    }

    void DoRoomLeave() {
        if (!room_active_) {
            return;
        }
        const bool was_hosting = room_host_;
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&room=" + UrlEncode(room_code_);
        PostNetTask(NetTaskKind::kRoomLeave, "/api/room/leave", form, true);
        ClearRoomUi();
        if (was_hosting) {
            // 保留内嵌服务器：房间里其他玩家可以继续，自己也可以用同一个 ID 回来续档。
            host_pending_ = false;
            pending_host_window_ = false;
            HideHostWindow();
        }
        net_status_ = T(L"left the rift", L"已离开时空");
        game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
        last_tick_ms_ = GetTickCount64();
        UpdateFreightDisplayLines();
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            embedder_.RefreshLayout(widget_window_, widget_size_);
        }
        RequestWidgetRedraw();
        RefreshOnlineUi();
    }

    void DoRoomStart(const std::string& tag = {}) {
        if (!room_active_) {
            return;
        }
        SetNetStatus(T(L"starting...", L"正在开始…"));
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&room=" + UrlEncode(room_code_);
        PostNetTask(NetTaskKind::kRoomStart, "/api/room/start", form, true, tag);
    }

    void PostRoomAction(const std::string& action, const std::string& arg) {
        if (!room_active_) {
            return;
        }
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&room=" + UrlEncode(room_code_) +
                           "&action=" + UrlEncode(action);
        if (!arg.empty()) {
            form += "&arg=" + UrlEncode(arg);
        }
        net_status_ = T(L"sent...", L"已发送…");
        PostNetTask(NetTaskKind::kRoomAction, "/api/room/action", form, true, action);
    }

    void ApplyRoomEntry(const std::string& body, bool show_panel = true) {
        room_active_ = true;
        room_code_ = NetField(body, "room");
        room_started_ = NetField(body, "started") == "1";
        room_host_ = NetField(body, "host") == "1";
        room_host_id_ = NetField(body, "host_id");
        room_host_name_ = NetField(body, "host_name");
        room_poll_pending_ = false;
        net_status_ = T(L"in rift ", L"已进入时空 ") + Utf8ToWide(room_code_);
        HideRoomWindow();
        RequestRoomState();
        if (show_panel) {
            ShowPopup(true);
        }
        RefreshHostWindow();
        RefreshOnlineUi();
    }

    // -- 加入时空窗口 ---------------------------------------------------------

    void SetFocusToRoomInput() {
        if (room_code_edit_ != nullptr && IsWindow(room_code_edit_)) {
            SetFocus(room_code_edit_);
            SendMessageW(room_code_edit_, EM_SETSEL, 0, -1);
        }
    }

    void ShowRoomWindow(const std::wstring& code) {
        if (room_window_ != nullptr && IsWindow(room_window_)) {
            SetWindowTextW(room_code_edit_, code.c_str());
            SetWindowTextW(room_name_edit_, Utf8ToWide(player_name_).c_str());
            SetRoomStatus(T(L"Paste an invite link, or type the rift code.",
                            L"粘贴邀请链接，或直接输入房间码。"));
            ShowWindow(room_window_, SW_SHOWNORMAL);
            SetForegroundWindow(room_window_);
            SetFocusToRoomInput();
            return;
        }
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int width = ScaleByDpi(current_dpi_, kRoomWindowWidth);
        const int height = ScaleByDpi(current_dpi_, kRoomWindowHeight);
        const int x = work.left + ((work.right - work.left) - width) / 2;
        const int y = work.top + ((work.bottom - work.top) - height) / 2;
        room_window_ = CreateWindowExW(WS_EX_TOPMOST, kRoomClassName,
                                       T(L"Stellar Lord - Join Rift", L"星际领主 · 加入时空"),
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width,
                                       height, nullptr, nullptr, instance_handle_, this);
        if (room_window_ == nullptr) {
            return;
        }
        SetWindowTextW(room_code_edit_, code.c_str());
        SetWindowTextW(room_name_edit_, Utf8ToWide(player_name_).c_str());
        ShowWindow(room_window_, SW_SHOWNORMAL);
        SetForegroundWindow(room_window_);
        SetFocusToRoomInput();
    }

    void HideRoomWindow() {
        if (room_window_ != nullptr && IsWindow(room_window_)) {
            ShowWindow(room_window_, SW_HIDE);
        }
    }

    void CreateRoomChildren(HWND window_handle) {
        RECT client{};
        GetClientRect(window_handle, &client);
        HFONT font = popup_font_ != nullptr ? popup_font_
                                            : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        const int pad = ScaleByDpi(current_dpi_, 14);
        const int label_width = ScaleByDpi(current_dpi_, 96);
        const int row_height = ScaleByDpi(current_dpi_, 24);
        const int gap = ScaleByDpi(current_dpi_, 10);
        const int button_width = ScaleByDpi(current_dpi_, 88);
        const int button_height = ScaleByDpi(current_dpi_, 28);
        const int inner_width = (client.right - client.left) - pad * 2;

        auto make_label = [&](const wchar_t* text, int x, int y, int w, int h) {
            HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x,
                                         y, w, h, window_handle, nullptr, instance_handle_,
                                         nullptr);
            SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return label;
        };

        int y = pad;
        make_label(T(L"Invite / code", L"邀请链接/码"), pad, y, label_width, row_height);
        room_code_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            pad + label_width, y, inner_width - label_width, row_height, window_handle, nullptr,
            instance_handle_, nullptr);

        y += row_height + gap;
        make_label(T(L"Nickname", L"昵称"), pad, y, label_width, row_height);
        room_name_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            pad + label_width, y, inner_width - label_width, row_height, window_handle, nullptr,
            instance_handle_, nullptr);

        y += row_height + gap;
        const int button_y = (client.bottom - client.top) - pad - button_height;
        room_status_text_ = make_label(
            T(L"Paste the invite link the host sent you, or type the 4-12 character rift code.",
              L"粘贴房主发来的邀请链接，或输入 4-12 位房间码。"),
            pad, y, inner_width, std::max(row_height, button_y - y - gap));

        const int cancel_x = (client.right - client.left) - pad - button_width;
        const int join_x = cancel_x - gap - button_width;
        const int paste_width = ScaleByDpi(current_dpi_, 112);
        const int paste_x = join_x - gap - paste_width;

        CreateWindowExW(0, L"BUTTON", T(L"Paste & join", L"粘贴并加入"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, paste_x, button_y,
                        paste_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kRoomPasteCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Join", L"加入"),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, join_x, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kRoomJoinCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Cancel", L"取消"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, cancel_x, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kRoomCancelCommandId)),
                        instance_handle_, nullptr);

        SendMessageW(room_code_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(room_name_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(room_window_, DM_SETDEFID, kRoomJoinCommandId, 0);
    }

    void SubmitRoomJoin() {
        if (room_code_edit_ == nullptr || !IsWindow(room_code_edit_)) {
            return;
        }
        wchar_t code_buffer[512]{};
        wchar_t name_buffer[64]{};
        GetWindowTextW(room_code_edit_, code_buffer, static_cast<int>(std::size(code_buffer)));
        GetWindowTextW(room_name_edit_, name_buffer, static_cast<int>(std::size(name_buffer)));

        InviteInfo invite;
        std::wstring parse_error;
        if (!ParseInviteText(TrimWide(code_buffer), invite, &parse_error)) {
            SetRoomStatus(parse_error);
            return;
        }

        std::wstring name = TrimWide(name_buffer);
        if (name.empty() || name.size() > 16) {
            SetRoomStatus(T(L"Nickname must be 1-16 characters.", L"昵称需为 1-16 个字符。"));
            return;
        }

        if (!invite.host.empty()) {
            app_config_.server_host = invite.host;
        }
        if (invite.port != 0) {
            app_config_.server_port = invite.port;
        }
        player_name_ = WideToUtf8(name);
        app_config_.nickname = player_name_;
        SaveAppConfig(app_config_);
        SetRoomStatus(T(L"Joining the rift...", L"正在加入时空…"));
        DoRoomJoin(invite.room);
    }

    void PasteInviteAndJoin() {
        if (room_code_edit_ == nullptr || !IsWindow(room_code_edit_)) {
            return;
        }
        std::wstring text;
        if (!ReadClipboardText(controller_window_, text) || TrimWide(text).empty()) {
            SetRoomStatus(T(L"Clipboard is empty.", L"剪贴板里没有内容。"));
            return;
        }
        SetWindowTextW(room_code_edit_, TrimWide(text).c_str());
        SetFocusToRoomInput();
        InviteInfo invite;
        std::wstring parse_error;
        if (!ParseInviteText(TrimWide(text), invite, &parse_error)) {
            SetRoomStatus(parse_error);
            return;
        }
        SubmitRoomJoin();
    }

    void SetRoomStatus(const std::wstring& text) {
        if (room_status_text_ != nullptr && IsWindow(room_status_text_)) {
            SetWindowTextW(room_status_text_, text.c_str());
        }
    }

    static LRESULT CALLBACK RoomWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                           LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->room_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_CREATE:
            app->CreateRoomChildren(window_handle);
            return 0;
        case WM_COMMAND:
            if (HIWORD(w_param) == BN_CLICKED) {
                const UINT id = LOWORD(w_param);
                if (id == kRoomJoinCommandId) {
                    app->SubmitRoomJoin();
                    return 0;
                }
                if (id == kRoomPasteCommandId) {
                    app->PasteInviteAndJoin();
                    return 0;
                }
                if (id == kRoomCancelCommandId) {
                    app->HideRoomWindow();
                    return 0;
                }
            }
            break;
        case WM_CLOSE:
            app->HideRoomWindow();
            return 0;
        case WM_NCDESTROY:
            if (app->room_window_ == window_handle) {
                app->room_window_ = nullptr;
                app->room_code_edit_ = nullptr;
                app->room_name_edit_ = nullptr;
                app->room_status_text_ = nullptr;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    void ParseRoomList(const std::string& body) {
        room_list_.clear();
        for (const std::string& line : NetFields(body, "room")) {
            const std::vector<std::string> parts = SplitPipe(line);
            if (parts.size() < 4) {
                continue;
            }
            RoomListEntry entry;
            entry.code = parts[0];
            entry.host_name = parts[1];
            entry.players = std::atoi(parts[2].c_str());
            entry.started = parts[3] == "1";
            room_list_.push_back(entry);
        }
    }

    void ParseRoomState(const std::string& body) {
        room_code_ = NetField(body, "room");
        room_started_ = NetField(body, "started") == "1";
        room_host_ = NetField(body, "host") == "1";
        room_host_id_ = NetField(body, "host_id");
        room_host_name_ = NetField(body, "host_name");
        const std::string audit_text = NetField(body, "audit");
        if (!audit_text.empty()) {
            const double new_audit = std::atof(audit_text.c_str());
            const int new_tier = std::atoi(NetField(body, "audit_tier").c_str());
            if (room_started_ && new_tier > room_audit_tier_) {
                action_report_ = AuditFlavor(new_tier);
                action_report_until_ms_ = GetTickCount64() + 10000;
            }
            room_audit_ = std::clamp(new_audit, 0.0, 100.0);
            room_audit_tier_ = std::clamp(new_tier, 0, 3);
        }
        room_members_.clear();
        for (const std::string& line : NetFields(body, "member")) {
            const std::vector<std::string> parts = SplitPipe(line);
            if (parts.size() < 8) {
                continue;
            }
            RoomMemberInfo member;
            member.id = parts[0];
            member.name = parts[1];
            member.score = std::atof(parts[2].c_str());
            member.fleet = std::atoi(parts[3].c_str());
            member.colonies = std::atoi(parts[4].c_str());
            member.capital = std::atoi(parts[5].c_str());
            member.online = parts[6] == "1";
            member.me = parts[7] == "1";
            if (parts.size() >= 14) {
                member.tokens = std::atof(parts[8].c_str());
                member.influence = std::atof(parts[9].c_str());
                member.relics = std::atoi(parts[10].c_str());
                member.cores = std::atoi(parts[11].c_str());
                member.data_cores = std::atoi(parts[12].c_str());
                member.reputation = std::atoi(parts[13].c_str());
            }
            room_members_.push_back(member);
        }

        // 服务端把玩家自己的状态逐行放在 s= 前缀后面，客户端直接反序列化。
        std::string state_text;
        size_t pos = 0;
        while (pos < body.size()) {
            size_t end = body.find('\n', pos);
            if (end == std::string::npos) {
                end = body.size();
            }
            std::string line = body.substr(pos, end - pos);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.compare(0, 2, "s=") == 0) {
                state_text += line.substr(2);
                state_text.push_back('\n');
            }
            pos = end + 1;
        }
        if (room_started_ && !state_text.empty()) {
            stellar::GameState loaded{};
            if (stellar::DeserializeState(state_text, loaded)) {
                game_ = loaded;
            }
        }
        last_room_poll_ms_ = GetTickCount64();
        room_poll_pending_ = false;
        UpdateFreightDisplayLines();
        RequestWidgetRedraw();
        RefreshHostWindow();
        RefreshOnlineUi();
    }

    void AbortHostStart(const std::wstring& message) {
        const bool had_room = room_active_;
        if (had_room) {
            ClearRoomUi();
            game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
            last_tick_ms_ = GetTickCount64();
            UpdateFreightDisplayLines();
            RequestWidgetRedraw();
        }
        host_pending_ = false;
        pending_host_window_ = false;
        rift_server::Stop();
        host_port_ = 0;
        host_lan_ip_.clear();
        net_status_ = message;
        RefreshHostWindow();
        RefreshOnlineUi();
    }

    std::wstring ActionLabel(const std::string& action) const {
        if (action == "build") return T(L"Facility upgraded.", L"设施已升级。");
        if (action == "capital") return T(L"Capital upgraded.", L"首都已升级。");
        if (action == "research") return T(L"Research completed.", L"研究完成。");
        if (action == "fleet") return T(L"Squadron built.", L"舰队已建造。");
        if (action == "colonize") return T(L"Colony founded.", L"新殖民地已建立。");
        if (action == "colony") return T(L"Colony upgraded.", L"殖民地已升级。");
        if (action == "faction") return T(L"Doctrine locked.", L"治国路线已锁定。");
        if (action == "wonder") return T(L"Wonder completed.", L"奇观已建成。");
        if (action == "resolve_event") return T(L"Event resolved.", L"事件已处理。");
        return T(L"done", L"已完成");
    }

    void HandleNetResult(NetTaskResult* raw) {
        std::unique_ptr<NetTaskResult> result(raw);
        if (result == nullptr) {
            return;
        }
        const NetResponse& net = result->response;

        if (!net.transport_ok) {
            if (result->kind == NetTaskKind::kRoomState) {
                room_poll_pending_ = false;
            }
            std::wstring reason;
            if (net.error == "connect") {
                reason = T(L"server unreachable", L"无法连接服务器");
            } else if (net.error == "dns") {
                reason = T(L"bad server address", L"服务器地址无效");
            } else {
                reason = T(L"network error", L"网络错误");
            }
            if (host_pending_ && (result->kind == NetTaskKind::kRoomCreate ||
                                  result->kind == NetTaskKind::kRoomStart)) {
                AbortHostStart(std::wstring(T(L"Host start failed: ", L"开启主机失败：")) + reason);
                return;
            }
            SetNetStatus(reason);
            SetRoomStatus(reason);
            return;
        }

        const std::string server_version = NetField(net.body, "v");
        if (server_version != std::to_string(rift_server::kProtocolVersion)) {
            if (result->kind == NetTaskKind::kRoomState) {
                room_poll_pending_ = false;
            }
            const std::wstring text = NetErrorText("version");
            if (host_pending_) {
                AbortHostStart(std::wstring(T(L"Host start failed: ", L"开启主机失败：")) + text);
            } else {
                SetNetStatus(text);
                SetRoomStatus(text);
            }
            return;
        }

        const std::string ok = NetField(net.body, "ok");
        const std::string err = NetField(net.body, "err");
        switch (result->kind) {
        case NetTaskKind::kRoomState: {
            room_poll_pending_ = false;
            if (net.status == 200 && ok == "1") {
                ParseRoomState(net.body);
            } else if (err == "no_room") {
                const bool was_hosting = room_host_;
                ClearRoomUi();
                if (was_hosting) {
                    rift_server::Stop();
                    host_port_ = 0;
                    host_lan_ip_.clear();
                    host_pending_ = false;
                    pending_host_window_ = false;
                }
                net_status_ = T(L"the rift was closed", L"时空已关闭");
                game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
                last_tick_ms_ = GetTickCount64();
                RefreshHostWindow();
                RefreshOnlineUi();
            } else {
                SetNetStatus(NetErrorText(err));
            }
            return;
        }
        case NetTaskKind::kRoomCreate:
        case NetTaskKind::kRoomJoin: {
            const bool host_entry = result->tag == "host";
            if (net.status == 200 && ok == "1") {
                ApplyRoomEntry(net.body, !host_entry);
                if (host_entry) {
                    net_status_ = T(L"starting rift...", L"正在开局…");
                    DoRoomStart("host-start");
                }
            } else {
                const std::wstring text = NetErrorText(err);
                if (host_entry || host_pending_) {
                    AbortHostStart(std::wstring(T(L"Host start failed: ", L"开启主机失败：")) +
                                   text);
                } else {
                    SetNetStatus(text);
                    SetRoomStatus(text);
                }
            }
            return;
        }
        case NetTaskKind::kRoomLeave:
            return;
        case NetTaskKind::kRoomStart: {
            if (net.status == 200 && ok == "1") {
                if (result->tag == "host-start") {
                    host_pending_ = false;
                    const bool show_window = pending_host_window_;
                    pending_host_window_ = false;
                    net_status_ = T(L"hosting ", L"主机已开启 ") + Utf8ToWide(room_code_);
                    if (show_window) {
                        ShowHostWindow();
                    } else {
                        ShowPopup(true);
                    }
                    RefreshHostWindow();
                } else {
                    net_status_ = T(L"game started", L"对局已开始");
                }
                RequestRoomState();
            } else {
                const std::wstring text = NetErrorText(err);
                if (result->tag == "host-start" || host_pending_) {
                    AbortHostStart(std::wstring(T(L"Host start failed: ", L"开启主机失败：")) +
                                   text);
                } else {
                    SetNetStatus(text);
                }
            }
            return;
        }
        case NetTaskKind::kRoomAction: {
            if (net.status == 200 && ok == "1") {
                if (NetField(net.body, "battle") == "1") {
                    stellar::BattleResult battle;
                    battle.won = NetField(net.body, "won") == "1";
                    battle.captured = NetField(net.body, "captured") == "1";
                    battle.attacker_loss = std::atoi(NetField(net.body, "loss").c_str());
                    battle.loot_minerals = std::atof(NetField(net.body, "loot_minerals").c_str());
                    battle.loot_alloy = std::atof(NetField(net.body, "loot_alloy").c_str());
                    battle.loot_research = std::atof(NetField(net.body, "loot_research").c_str());
                    ReportBattle(battle);
                } else {
                    net_status_ = ActionLabel(result->tag);
                }
                RequestRoomState();
            } else {
                SetNetStatus(NetErrorText(err));
            }
            return;
        }
        case NetTaskKind::kRoomList: {
            if (net.status == 200 && ok == "1") {
                ParseRoomList(net.body);
                net_status_ = room_list_.empty()
                                  ? std::wstring(T(L"no open rifts", L"暂无公开时空"))
                                  : T(L"found ", L"找到 ") +
                                        std::to_wstring(room_list_.size()) +
                                        T(L" rifts", L" 个时空");
            } else {
                net_status_ = NetErrorText(err);
            }
            RefreshOnlineUi();
            return;
        }
        }
    }

    // -- 菜单与设置 ---------------------------------------------------------

    void ShowContextMenu(POINT screen_point) {
        HidePopup();
        HMENU menu = CreatePopupMenu();
        HMENU mode_menu = CreatePopupMenu();
        HMENU popup_menu = CreatePopupMenu();
        HMENU interval_menu = CreatePopupMenu();
        HMENU hotkey_menu = CreatePopupMenu();
        HMENU language_menu = CreatePopupMenu();

        const PopupActivationMode mode = CurrentPopupMode();
        AppendMenuW(mode_menu, MF_STRING | (freight_mode_ ? 0 : MF_CHECKED), kModeMonitorCommandId,
                    T(L"System monitor", L"系统监视"));
        AppendMenuW(mode_menu, MF_STRING | (freight_mode_ ? MF_CHECKED : 0), kModeLordCommandId,
                    T(L"Stellar Lord", L"星际领主"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mode_menu),
                    T(L"Mode", L"模式"));

        AppendMenuW(popup_menu, MF_STRING | (mode == PopupActivationMode::kHover ? MF_CHECKED : 0),
                    kPopupHoverCommandId, T(L"Hover", L"悬停弹出"));
        AppendMenuW(popup_menu, MF_STRING | (mode == PopupActivationMode::kClick ? MF_CHECKED : 0),
                    kPopupClickCommandId, T(L"Click", L"点击弹出"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(popup_menu),
                    T(L"Popup", L"弹窗方式"));

        AppendMenuW(interval_menu,
                    MF_STRING | (app_config_.sample_interval_seconds == 1 ? MF_CHECKED : 0),
                    kInterval1sCommandId, L"1s");
        AppendMenuW(interval_menu,
                    MF_STRING | (app_config_.sample_interval_seconds == 2 ? MF_CHECKED : 0),
                    kInterval2sCommandId, L"2s");
        AppendMenuW(interval_menu,
                    MF_STRING | (app_config_.sample_interval_seconds == 5 ? MF_CHECKED : 0),
                    kInterval5sCommandId, L"5s");
        AppendMenuW(interval_menu,
                    MF_STRING | (app_config_.sample_interval_seconds == 10 ? MF_CHECKED : 0),
                    kInterval10sCommandId, L"10s");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(interval_menu),
                    T(L"Refresh interval", L"刷新间隔"));

        AppendMenuW(hotkey_menu,
                    MF_STRING | (app_config_.toggle_hotkey == ToggleHotkey::kAltQ ? MF_CHECKED : 0),
                    kHotkeyAltQCommandId, L"Alt+Q");
        AppendMenuW(hotkey_menu,
                    MF_STRING | (app_config_.toggle_hotkey == ToggleHotkey::kAltS ? MF_CHECKED : 0),
                    kHotkeyAltSCommandId, L"Alt+S");
        AppendMenuW(hotkey_menu,
                    MF_STRING | (app_config_.toggle_hotkey == ToggleHotkey::kAltM ? MF_CHECKED : 0),
                    kHotkeyAltMCommandId, L"Alt+M");
        AppendMenuW(hotkey_menu,
                    MF_STRING |
                        (app_config_.toggle_hotkey == ToggleHotkey::kCtrlAltQ ? MF_CHECKED : 0),
                    kHotkeyCtrlAltQCommandId, L"Ctrl+Alt+Q");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hotkey_menu),
                    T(L"Hotkey", L"快捷键"));

        AppendMenuW(language_menu,
                    MF_STRING | (g_language == UiLanguage::kEnglish ? MF_CHECKED : 0),
                    kLanguageEnglishCommandId, L"English");
        AppendMenuW(language_menu,
                    MF_STRING | (g_language == UiLanguage::kChinese ? MF_CHECKED : 0),
                    kLanguageChineseCommandId, L"中文");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language_menu),
                    T(L"Language", L"语言"));

        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kHostCommandId, T(L"Host this PC", L"开启本机主机"));
        AppendMenuW(menu, MF_STRING, kInviteCommandId, T(L"Copy invite link", L"复制邀请链接"));
        AppendMenuW(menu, MF_STRING, kOnlineCommandId, T(L"Join Rift", L"加入时空"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0), kAutoStartCommandId,
                    T(L"Launch at startup", L"开机启动"));
        AppendMenuW(menu, MF_STRING, kHelpCommandId, T(L"Help", L"帮助"));
        AppendMenuW(menu, MF_STRING, kResetSaveCommandId, T(L"Reset empire", L"重置帝国"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExitCommandId, T(L"Exit", L"退出"));

        SetForegroundWindow(controller_window_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN, screen_point.x, screen_point.y, 0,
                       controller_window_, nullptr);
        DestroyMenu(menu);
    }

    std::wstring HotkeyText() const {
        switch (app_config_.toggle_hotkey) {
        case ToggleHotkey::kAltS: return L"Alt+S";
        case ToggleHotkey::kAltM: return L"Alt+M";
        case ToggleHotkey::kCtrlAltQ: return L"Ctrl+Alt+Q";
        case ToggleHotkey::kAltQ:
        default: return L"Alt+Q";
        }
    }

    void ShowHelp() {
        const std::wstring hotkey = HotkeyText();
        std::wstring text;
        if (g_language == UiLanguage::kEnglish) {
            text = L"Looks like a system monitor; the taskbar widget is the game.\n"
                   L"Press " + hotkey +
                   L" to switch between the monitor panel and the empire panel.\n"
                   L"Hover or click the taskbar widget to open the panel.\n\n"
                   L"Empire panel: 1-8 switch tabs, up/down select, Enter confirms, Esc closes.\n"
                   L"Loop: resources -> population -> research -> fleet -> colonize -> capture -> develop.\n"
                   L"Trade tab: sell goods for ST, fulfil contracts, buy from the black market.\n"
                   L"Expedition tab: send 1-3 fleets to ruins, anomalies and drifting arks.\n"
                   L"Events tab: choose a doctrine, answer event cards, build wonders, relieve audit pressure.\n"
                   L"Rift tab: host this PC to get one invite link, or paste a friend's link to join.\n"
                   L"Everyone starts as a commoner; the host starts the match for all players.\n"
                   L"Leave and rejoin with the same ID within 30 minutes to keep your empire.\n"
                   L"Both sides must run the same game version; old versions are rejected.\n"
                   L"Room progress lives only in server memory and is never written to disk.\n"
                   L"Right-click the widget for mode, popup style, hotkey and language.";
        } else {
            text = L"外观是系统监视器，任务栏上的控件其实是游戏。\n"
                   L"按 " + hotkey + L" 在「监视器面板」和「领主面板」之间切换。\n"
                   L"鼠标悬停或点击任务栏控件即可打开面板。\n\n"
                   L"领主面板：1-8 切换标签，上下键选择，回车确认，Esc 关闭。\n"
                   L"核心循环：资源 → 人口 → 科研 → 舰队 → 殖民 → 占领 → 开发。\n"
                   L"贸易页：卖资源换星际代币、交付合同、在黑市买稀有货。\n"
                   L"远征页：派 1-3 支舰队去遗迹带、深空异常和漂流方舟。\n"
                   L"事件页：选治国路线、处理事件卡、建造奇观、缓解全房间审计压力。\n"
                   L"联机页：开启本机主机即可得到一条邀请链接；朋友粘贴链接直接加入。\n"
                   L"所有人以平民身份同时开局，由房主统一开始，迟到玩家会自动追赶进度。\n"
                   L"离开后 30 分钟内用同一玩家 ID 重新加入，可继续原来的帝国。\n"
                   L"双方必须是同一游戏版本，旧版本会被拒绝联机。\n"
                   L"进度只存在服务端内存里，不写本机文件。\n"
                   L"右键控件可切换模式、弹窗方式、快捷键与语言。";
        }
        MessageBoxW(controller_window_, text.c_str(),
                    T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONINFORMATION);
    }

    bool SetPopupMode(PopupActivationMode mode) {
        if (freight_mode_) {
            app_config_.lord_popup_mode = mode;
        } else {
            app_config_.monitor_popup_mode = mode;
        }
        return SaveAppConfig(app_config_);
    }

    bool SetSampleInterval(unsigned int seconds) {
        app_config_.sample_interval_seconds = seconds;
        if (controller_window_ != nullptr) {
            SetTimer(controller_window_, kSampleTimerId, GetSampleTimerIntervalMs(seconds), nullptr);
        }
        return SaveAppConfig(app_config_);
    }

    bool SetHotkey(ToggleHotkey hotkey) {
        app_config_.toggle_hotkey = hotkey;
        RegisterToggleHotkey();
        return SaveAppConfig(app_config_);
    }

    bool SetLanguage(UiLanguage language) {
        g_language = language;
        app_config_.language = language;
        ApplyWidgetLines();
        UpdateMonitorDisplayLines(last_snapshot_);
        UpdateFreightDisplayLines();
        RefreshFontAndSize();
        if (embedder_.IsAttached()) {
            embedder_.RefreshLayout(widget_window_, widget_size_);
        }
        RequestWidgetRedraw();
        if (popup_visible_) {
            RefreshFreightRows();
            RequestPopupRedraw();
        }
        return SaveAppConfig(app_config_);
    }

    void ApplyMenuCommand(UINT command_id) {
        switch (command_id) {
        case kExitCommandId:
            Shutdown();
            PostQuitMessage(0);
            break;
        case kModeMonitorCommandId:
            if (freight_mode_) ToggleMode();
            break;
        case kModeLordCommandId:
            if (!freight_mode_) ToggleMode();
            break;
        case kPopupHoverCommandId:
            SetPopupMode(PopupActivationMode::kHover);
            break;
        case kPopupClickCommandId:
            SetPopupMode(PopupActivationMode::kClick);
            break;
        case kInterval1sCommandId:
            SetSampleInterval(1);
            break;
        case kInterval2sCommandId:
            SetSampleInterval(2);
            break;
        case kInterval5sCommandId:
            SetSampleInterval(5);
            break;
        case kInterval10sCommandId:
            SetSampleInterval(10);
            break;
        case kHotkeyAltQCommandId:
            SetHotkey(ToggleHotkey::kAltQ);
            break;
        case kHotkeyAltSCommandId:
            SetHotkey(ToggleHotkey::kAltS);
            break;
        case kHotkeyAltMCommandId:
            SetHotkey(ToggleHotkey::kAltM);
            break;
        case kHotkeyCtrlAltQCommandId:
            SetHotkey(ToggleHotkey::kCtrlAltQ);
            break;
        case kLanguageEnglishCommandId:
            SetLanguage(UiLanguage::kEnglish);
            break;
        case kLanguageChineseCommandId:
            SetLanguage(UiLanguage::kChinese);
            break;
        case kAutoStartCommandId:
            SetAutoStartEnabled(!IsAutoStartEnabled());
            break;
        case kHelpCommandId:
            ShowHelp();
            break;
        case kResetSaveCommandId:
            ResetProgress();
            break;
        case kOnlineCommandId:
            if (!freight_mode_) {
                ToggleMode();
            }
            freight_tab_ = FreightTab::kOnline;
            freight_selected_ = -1;
            RefreshFreightRows();
            ShowPopup(true);
            if (room_active_) {
                RequestRoomState();
            } else {
                RefreshRoomList();
            }
            break;
        case kHostCommandId:
            StartHostMode(true);
            break;
        case kInviteCommandId:
            CopyInviteLink();
            break;
        default:
            break;
        }
    }

    // -- 开机启动 -----------------------------------------------------------

    static std::wstring AutoStartCommand() {
        wchar_t buffer[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
        return std::wstring(L"\"") + buffer + L"\"";
    }

    static bool IsAutoStartEnabled() {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return false;
        }
        wchar_t value[1024]{};
        DWORD size = sizeof(value);
        DWORD type = 0;
        const LONG result = RegQueryValueExW(key, kRunValueName, nullptr, &type,
                                             reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }

    static bool SetAutoStartEnabled(bool enabled) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryPath, 0, KEY_SET_VALUE, &key) !=
            ERROR_SUCCESS) {
            return false;
        }
        bool ok = true;
        if (enabled) {
            const std::wstring command = AutoStartCommand();
            ok = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) ==
                 ERROR_SUCCESS;
        } else {
            RegDeleteValueW(key, kRunValueName);
        }
        RegCloseKey(key);
        return ok;
    }

    // -- 快捷键 -------------------------------------------------------------

    struct HotkeySpec {
        UINT modifiers;
        UINT virtual_key;
    };

    HotkeySpec GetHotkeySpec() const {
        switch (app_config_.toggle_hotkey) {
        case ToggleHotkey::kAltS:
            return {MOD_ALT | MOD_NOREPEAT, 'S'};
        case ToggleHotkey::kAltM:
            return {MOD_ALT | MOD_NOREPEAT, 'M'};
        case ToggleHotkey::kCtrlAltQ:
            return {MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'};
        case ToggleHotkey::kAltQ:
        default:
            return {MOD_ALT | MOD_NOREPEAT, 'Q'};
        }
    }

    bool RegisterToggleHotkey() {
        UnregisterHotKey(controller_window_, kToggleModeHotkeyId);
        const HotkeySpec spec = GetHotkeySpec();
        return RegisterHotKey(controller_window_, kToggleModeHotkeyId, spec.modifiers,
                              spec.virtual_key) != FALSE;
    }

    // -- 窗口过程 -----------------------------------------------------------

    static MonitorApp* FromWindow(HWND window_handle) {
        return reinterpret_cast<MonitorApp*>(GetWindowLongPtrW(window_handle, GWLP_USERDATA));
    }

    static LRESULT CALLBACK ControllerWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                                 LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->controller_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }

        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_CREATE:
            if (!app->Initialize()) {
                return -1;
            }
            return 0;
        case WM_TIMER:
            switch (w_param) {
            case kSampleTimerId:
                app->SampleAndRefresh();
                break;
            case kLayoutTimerId:
                app->UpdateLayout();
                break;
            case kGameTickTimerId:
                app->TickGame();
                break;
            case kReattachTimerId:
                KillTimer(window_handle, kReattachTimerId);
                app->ReattachWidget(true);
                break;
            case kHoverHideTimerId:
                KillTimer(window_handle, kHoverHideTimerId);
                if (!app->IsCursorOverWidgetOrPopup()) {
                    app->HidePopup();
                }
                break;
            default:
                break;
            }
            return 0;
        case kSystemSampleReadyMessage:
            app->ReceiveSystemSample();
            return 0;
        case kNetResultMessage:
            app->HandleNetResult(reinterpret_cast<NetTaskResult*>(l_param));
            return 0;
        case kWidgetRedrawMessage:
            app->widget_redraw_pending_ = false;
            app->RedrawWidgetNow();
            return 0;
        case WM_HOTKEY:
            if (w_param == kToggleModeHotkeyId) {
                app->ToggleMode();
            }
            return 0;
        case WM_COMMAND:
            app->ApplyMenuCommand(LOWORD(w_param));
            return 0;
        case kTrayIconCallbackMessage:
            if (l_param == WM_LBUTTONUP || l_param == WM_LBUTTONDBLCLK) {
                app->ToggleMode();
            } else if (l_param == WM_RBUTTONUP || l_param == WM_CONTEXTMENU) {
                POINT point{};
                GetCursorPos(&point);
                app->ShowContextMenu(point);
            }
            return 0;
        case WM_CLOSE:
            app->Shutdown();
            DestroyWindow(window_handle);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }

        if (message == app->taskbar_created_message_) {
            app->ScheduleTaskbarReattach();
            return 0;
        }

        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK WidgetWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                             LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->widget_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintWidget();
            return 0;
        case WM_MOUSEMOVE:
            app->HandleHoverMove(window_handle);
            return 0;
        case WM_MOUSELEAVE:
            app->ArmHoverHideTimer();
            return 0;
        case WM_MOUSEWHEEL:
            if (app->freight_mode_) {
                app->SwitchTab(GET_WHEEL_DELTA_WPARAM(w_param) > 0 ? -1 : 1);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            app->HandleWidgetLeftButtonDown();
            return 0;
        case WM_LBUTTONUP:
            app->HandleWidgetLeftButtonUp();
            return 0;
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->widget_window_ == window_handle) {
                app->HidePopup();
                app->widget_window_ = nullptr;
                if (!app->is_shutting_down_ && app->controller_window_ != nullptr &&
                    IsWindow(app->controller_window_)) {
                    SetTimer(app->controller_window_, kReattachTimerId, kReattachDelayMs, nullptr);
                }
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    static LRESULT CALLBACK PopupWindowProc(HWND window_handle, UINT message, WPARAM w_param,
                                            LPARAM l_param) {
        MonitorApp* app = nullptr;
        if (message == WM_NCCREATE) {
            auto* create_struct = reinterpret_cast<CREATESTRUCTW*>(l_param);
            app = static_cast<MonitorApp*>(create_struct->lpCreateParams);
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->popup_window_ = window_handle;
        } else {
            app = FromWindow(window_handle);
        }
        if (app == nullptr) {
            return DefWindowProcW(window_handle, message, w_param, l_param);
        }

        switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            app->PaintPopup();
            return 0;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            app->HandlePopupMouseMove(window_handle, point);
            return 0;
        }
        case WM_SETCURSOR: {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window_handle, &point);
            if (app->HitTestPopupItem(point) != -1) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        case WM_MOUSELEAVE:
            app->popup_hover_row_ = -1;
            app->RequestPopupRedraw();
            app->ArmHoverHideTimer();
            return 0;
        case WM_MOUSEWHEEL:
            if (app->freight_mode_) {
                app->SwitchTab(GET_WHEEL_DELTA_WPARAM(w_param) > 0 ? -1 : 1);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            return 0;
        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            app->HandlePopupClick(point);
            return 0;
        }
        case WM_KEYDOWN:
            app->HandlePopupKey(w_param);
            return 0;
        case WM_KILLFOCUS:
            if (app->CurrentPopupMode() == PopupActivationMode::kClick) {
                // 只有光标确实离开了控件和弹窗才收起。之前只判断控件，
                // 导致点击弹窗内部时焦点变化会把弹窗关掉。
                if (!app->IsCursorOverWidgetOrPopup()) {
                    app->HidePopup();
                }
            } else {
                app->ArmHoverHideTimer();
            }
            return 0;
        case WM_CLOSE:
            app->HidePopup();
            return 0;
        case WM_RBUTTONUP: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            ClientToScreen(window_handle, &point);
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_CONTEXTMENU: {
            POINT point{GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param)};
            if (point.x == -1 && point.y == -1) {
                RECT rect{};
                GetWindowRect(window_handle, &rect);
                point.x = rect.left;
                point.y = rect.bottom;
            }
            app->ShowContextMenu(point);
            return 0;
        }
        case WM_NCDESTROY:
            if (app->popup_window_ == window_handle) {
                app->popup_window_ = nullptr;
                app->popup_visible_ = false;
            }
            SetWindowLongPtrW(window_handle, GWLP_USERDATA, 0);
            break;
        default:
            break;
        }
        return DefWindowProcW(window_handle, message, w_param, l_param);
    }

    // -----------------------------------------------------------------------
    // 成员
    // -----------------------------------------------------------------------

    HINSTANCE instance_handle_{nullptr};
    HWND controller_window_{nullptr};
    HWND widget_window_{nullptr};
    HWND popup_window_{nullptr};
    HFONT font_{nullptr};
    HFONT popup_font_{nullptr};
    HFONT popup_title_font_{nullptr};

    UINT current_dpi_{96};
    UINT taskbar_created_message_{0};
    SIZE widget_size_{};
    SIZE hover_popup_size_{};
    int text_line_height_{0};
    int popup_text_line_height_{0};
    int popup_title_line_height_{0};
    bool has_second_line_{true};

    bool is_shutting_down_{false};
    bool widget_redraw_pending_{false};
    bool popup_visible_{false};
    bool popup_keyboard_active_{false};
    bool click_popup_started_from_widget_{false};
    bool tray_icon_added_{false};
    HICON tray_icon_handle_{nullptr};
    ULONGLONG last_tick_ms_{0};

    AppConfig app_config_{};
    bool freight_mode_{false};
    int popup_hover_row_{-1};

    MetricsSnapshot last_snapshot_{};
    ProcessPopupSnapshot process_snapshot_{};

    // 任务栏文本
    std::wstring widget_line1_{L"CPU 0%  MEM 0%"};
    std::wstring widget_line2_{L"\u2191 0bps  \u2193 0bps"};
    std::vector<DisplayLines::Column> widget_columns_{};
    std::vector<int> widget_column_widths_{};
    std::wstring monitor_line1_{L"CPU 0%  MEM 0%"};
    std::wstring monitor_line2_{L"\u2191 0bps  \u2193 0bps"};
    std::vector<DisplayLines::Column> monitor_columns_{};
    std::wstring freight_line1_{L"PWR 0   MIN 0   FOOD 0"};
    std::wstring freight_line2_{L"POP 0/20   ALY 0   FLT 0"};

    // 游戏
    stellar::GameState game_{};
    FreightTab freight_tab_{FreightTab::kDevelopment};
    std::vector<FreightRow> freight_rows_{};
    int freight_selected_{-1};
    std::wstring action_report_{};
    ULONGLONG action_report_until_ms_{0};

    // 联机
    HWND server_window_{nullptr};
    HWND server_host_edit_{nullptr};
    HWND server_port_edit_{nullptr};
    HWND server_status_text_{nullptr};
    std::wstring net_status_{};

    // 加入时空
    HWND room_window_{nullptr};
    HWND room_code_edit_{nullptr};
    HWND room_name_edit_{nullptr};
    HWND room_status_text_{nullptr};
    std::string player_name_{};
    bool room_active_{false};
    bool room_started_{false};
    bool room_host_{false};
    bool room_poll_pending_{false};
    std::string room_code_{};
    std::string room_host_id_{};
    std::string room_host_name_{};
    double room_audit_{15.0};
    int room_audit_tier_{0};
    std::vector<RoomMemberInfo> room_members_{};
    std::vector<RoomListEntry> room_list_{};
    ULONGLONG last_room_poll_ms_{0};

    // 启动选择与内嵌主机
    HWND launch_window_{nullptr};
    std::wstring startup_invite_text_{};
    bool startup_server_{false};
    bool startup_client_{false};
    bool self_test_{false};

    HWND host_window_{nullptr};
    HWND host_invite_edit_{nullptr};
    HWND host_status_text_{nullptr};
    std::string host_lan_ip_{};
    unsigned int host_port_{0};
    bool host_pending_{false};
    bool pending_host_window_{false};

    BackgroundWorker<bool, SystemSampleResult> system_worker_{};
    TaskbarEmbedder embedder_{};
};

}  // namespace

}  // namespace minimal_taskbar_monitor

int APIENTRY wWinMain(HINSTANCE instance_handle, HINSTANCE, LPWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    minimal_taskbar_monitor::MonitorApp app;
    return app.Run(instance_handle, show_command);
}
