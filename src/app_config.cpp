#include "app_config.h"

#include <cctype>
#include <filesystem>
#include <string_view>

extern "C" BOOLEAN NTAPI SystemFunction036(PVOID RandomBuffer, ULONG RandomBufferLength);
#pragma comment(lib, "advapi32.lib")

namespace minimal_taskbar_monitor {

namespace {

constexpr wchar_t kConfigFileName[] = L"config.json";
constexpr wchar_t kSaveFileName[] = L"stellar_lord_save.txt";

bool ReadBoolValue(const std::string& content, std::string_view key, bool default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    size_t colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return default_value;
    }
    ++colon_pos;
    while (colon_pos < content.size() &&
           (content[colon_pos] == ' ' || content[colon_pos] == '\t' || content[colon_pos] == '\r' ||
            content[colon_pos] == '\n')) {
        ++colon_pos;
    }
    if (content.compare(colon_pos, 4, "true") == 0) {
        return true;
    }
    if (content.compare(colon_pos, 5, "false") == 0) {
        return false;
    }
    return default_value;
}

unsigned int ReadUnsignedIntValue(const std::string& content,
                                  std::string_view key,
                                  unsigned int default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return default_value;
    }
    size_t colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return default_value;
    }
    ++colon_pos;
    while (colon_pos < content.size() &&
           (content[colon_pos] == ' ' || content[colon_pos] == '\t' || content[colon_pos] == '\r' ||
            content[colon_pos] == '\n')) {
        ++colon_pos;
    }
    unsigned int value = 0;
    bool has_digit = false;
    while (colon_pos < content.size() && content[colon_pos] >= '0' && content[colon_pos] <= '9') {
        has_digit = true;
        value = value * 10u + static_cast<unsigned int>(content[colon_pos] - '0');
        ++colon_pos;
    }
    return has_digit ? value : default_value;
}

std::string ReadStringValue(const std::string& content,
                            std::string_view key,
                            std::string_view default_value) {
    const std::string quoted_key = "\"" + std::string(key) + "\"";
    const size_t key_pos = content.find(quoted_key);
    if (key_pos == std::string::npos) {
        return std::string(default_value);
    }
    size_t colon_pos = content.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return std::string(default_value);
    }
    ++colon_pos;
    while (colon_pos < content.size() &&
           (content[colon_pos] == ' ' || content[colon_pos] == '\t' || content[colon_pos] == '\r' ||
            content[colon_pos] == '\n')) {
        ++colon_pos;
    }
    if (colon_pos >= content.size() || content[colon_pos] != '"') {
        return std::string(default_value);
    }
    ++colon_pos;
    const size_t end_quote_pos = content.find('"', colon_pos);
    if (end_quote_pos == std::string::npos) {
        return std::string(default_value);
    }
    return content.substr(colon_pos, end_quote_pos - colon_pos);
}

std::string SerializeBool(const char* key, bool value) {
    return std::string("  \"") + key + "\": " + (value ? "true" : "false");
}

std::string SerializeUnsignedInt(const char* key, unsigned int value) {
    return std::string("  \"") + key + "\": " + std::to_string(value);
}

std::string SerializeString(const char* key, std::string_view value) {
    return std::string("  \"") + key + "\": \"" + std::string(value) + "\"";
}

unsigned int NormalizeSampleIntervalSeconds(unsigned int value) {
    switch (value) {
    case 1:
    case 2:
    case 5:
    case 10:
        return value;
    default:
        return 1;
    }
}

unsigned int NormalizeServerPort(unsigned int value) {
    return value == 0 || value > 65535 ? 8787 : value;
}

bool IsValidPlayerId(std::string_view value) {
    if (value.size() < 6 || value.size() > 32) {
        return false;
    }
    for (char ch : value) {
        const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// 本地身份：16 位十六进制随机串。用系统 CSPRNG（advapi32 的 SystemFunction036），
// 猜中别人 ID 的概率足够低，也就不需要账号密码了。
std::string GeneratePlayerId() {
    unsigned char bytes[8]{};
    if (!SystemFunction036(bytes, static_cast<ULONG>(sizeof(bytes)))) {
        const unsigned long long seed = static_cast<unsigned long long>(GetTickCount64()) ^
                                        (static_cast<unsigned long long>(GetCurrentProcessId()) << 32);
        for (int i = 0; i < 8; ++i) {
            bytes[i] = static_cast<unsigned char>((seed >> (i * 8)) & 0xFF);
        }
    }
    static const char kHex[] = "0123456789abcdef";
    std::string id;
    id.reserve(sizeof(bytes) * 2);
    for (unsigned char value : bytes) {
        id.push_back(kHex[value >> 4]);
        id.push_back(kHex[value & 0x0F]);
    }
    return id;
}

NetworkDisplayUnit ParseNetworkDisplayUnit(std::string_view value) {
    return value == "bytes" ? NetworkDisplayUnit::kBytesPerSecond
                            : NetworkDisplayUnit::kBitsPerSecond;
}

std::string_view SerializeNetworkDisplayUnit(NetworkDisplayUnit value) {
    return value == NetworkDisplayUnit::kBytesPerSecond ? "bytes" : "bits";
}

PopupActivationMode ParsePopupActivationMode(std::string_view value) {
    return value == "click" ? PopupActivationMode::kClick : PopupActivationMode::kHover;
}

std::string_view SerializePopupActivationMode(PopupActivationMode value) {
    return value == PopupActivationMode::kClick ? "click" : "hover";
}

ToggleHotkey ParseToggleHotkey(std::string_view value) {
    if (value == "alt_s") return ToggleHotkey::kAltS;
    if (value == "alt_m") return ToggleHotkey::kAltM;
    if (value == "ctrl_alt_q") return ToggleHotkey::kCtrlAltQ;
    return ToggleHotkey::kAltQ;
}

std::string_view SerializeToggleHotkey(ToggleHotkey value) {
    switch (value) {
    case ToggleHotkey::kAltS: return "alt_s";
    case ToggleHotkey::kAltM: return "alt_m";
    case ToggleHotkey::kCtrlAltQ: return "ctrl_alt_q";
    case ToggleHotkey::kAltQ:
    default: return "alt_q";
    }
}

UiLanguage ParseUiLanguage(std::string_view value) {
    return value == "en" ? UiLanguage::kEnglish : UiLanguage::kChinese;
}

std::string_view SerializeUiLanguage(UiLanguage value) {
    return value == UiLanguage::kEnglish ? "en" : "zh_cn";
}

AppMode ParseAppMode(std::string_view value) {
    return value == "monitor" ? AppMode::kMonitor : AppMode::kLord;
}

std::string_view SerializeAppMode(AppMode value) {
    return value == AppMode::kMonitor ? "monitor" : "lord";
}

std::filesystem::path ExecutableDirectory() {
    std::wstring executable_path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable_path.data(),
                                            static_cast<DWORD>(executable_path.size()));
    if (length == 0 || length >= executable_path.size()) {
        return std::filesystem::path(L".");
    }
    executable_path.resize(length);
    return std::filesystem::path(executable_path).parent_path();
}

}  // namespace

std::wstring GetAppConfigPath() {
    return (ExecutableDirectory() / kConfigFileName).wstring();
}

std::wstring GetSaveFilePath() {
    return (ExecutableDirectory() / kSaveFileName).wstring();
}

AppConfig LoadAppConfig() {
    AppConfig config{};
    // 首次启动（或配置损坏）也要有身份，否则云存档没有键。
    auto ensure_player_id = [&config]() {
        if (!IsValidPlayerId(config.player_id)) {
            config.player_id = GeneratePlayerId();
        }
    };
    const std::filesystem::path config_path = GetAppConfigPath();
    HANDLE file_handle = CreateFileW(config_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        ensure_player_id();
        return config;
    }

    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file_handle, &file_size) || file_size.QuadPart <= 0 ||
        file_size.QuadPart > 64 * 1024) {
        CloseHandle(file_handle);
        ensure_player_id();
        return config;
    }

    std::string content(static_cast<size_t>(file_size.QuadPart), '\0');
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(file_handle, content.data(), static_cast<DWORD>(content.size()),
                                  &bytes_read, nullptr);
    CloseHandle(file_handle);
    if (!read_ok) {
        ensure_player_id();
        return config;
    }
    content.resize(bytes_read);

    config.visible_metrics.show_cpu =
        ReadBoolValue(content, "show_cpu", config.visible_metrics.show_cpu);
    config.visible_metrics.show_memory =
        ReadBoolValue(content, "show_memory", config.visible_metrics.show_memory);
    config.visible_metrics.show_upload =
        ReadBoolValue(content, "show_upload", config.visible_metrics.show_upload);
    config.visible_metrics.show_download =
        ReadBoolValue(content, "show_download", config.visible_metrics.show_download);
    config.visible_metrics.show_gpu =
        ReadBoolValue(content, "show_gpu", config.visible_metrics.show_gpu);
    config.visible_metrics.show_disk_read =
        ReadBoolValue(content, "show_disk_read", config.visible_metrics.show_disk_read);
    config.visible_metrics.show_disk_write =
        ReadBoolValue(content, "show_disk_write", config.visible_metrics.show_disk_write);
    config.network_display_unit =
        ParseNetworkDisplayUnit(ReadStringValue(content, "network_unit", "bits"));
    config.monitor_popup_mode =
        ParsePopupActivationMode(ReadStringValue(content, "monitor_popup_mode", "hover"));
    config.lord_popup_mode =
        ParsePopupActivationMode(ReadStringValue(content, "lord_popup_mode", "click"));
    config.toggle_hotkey = ParseToggleHotkey(ReadStringValue(content, "toggle_hotkey", "alt_q"));
    config.language = ParseUiLanguage(ReadStringValue(content, "language", "zh_cn"));
    config.mode = ParseAppMode(ReadStringValue(content, "mode", "lord"));
    config.sample_interval_seconds = NormalizeSampleIntervalSeconds(
        ReadUnsignedIntValue(content, "sample_interval_seconds", config.sample_interval_seconds));
    config.taskbar_monitor_index =
        ReadUnsignedIntValue(content, "taskbar_monitor_index", config.taskbar_monitor_index);
    config.auto_start = ReadBoolValue(content, "auto_start", config.auto_start);
    config.server_host = ReadStringValue(content, "server_host", config.server_host);
    if (config.server_host.empty()) {
        config.server_host = "127.0.0.1";
    }
    config.server_port =
        NormalizeServerPort(ReadUnsignedIntValue(content, "server_port", config.server_port));
    config.player_id = ReadStringValue(content, "player_id", config.player_id);
    if (!IsValidPlayerId(config.player_id)) {
        config.player_id = GeneratePlayerId();
    }
    config.nickname = ReadStringValue(content, "nickname", config.nickname);
    if (config.nickname.size() > 48) {
        config.nickname.resize(48);
    }
    return config;
}
bool SaveAppConfig(const AppConfig& config) {
    const std::filesystem::path config_path = GetAppConfigPath();
    std::error_code error_code;
    const std::filesystem::path parent_path = config_path.parent_path();
    if (!parent_path.empty()) {
        std::filesystem::create_directories(parent_path, error_code);
    }
    if (error_code) {
        return false;
    }

    std::string content = "{\n";
    content += SerializeBool("show_cpu", config.visible_metrics.show_cpu) + ",\n";
    content += SerializeBool("show_memory", config.visible_metrics.show_memory) + ",\n";
    content += SerializeBool("show_upload", config.visible_metrics.show_upload) + ",\n";
    content += SerializeBool("show_download", config.visible_metrics.show_download) + ",\n";
    content += SerializeBool("show_gpu", config.visible_metrics.show_gpu) + ",\n";
    content += SerializeBool("show_disk_read", config.visible_metrics.show_disk_read) + ",\n";
    content += SerializeBool("show_disk_write", config.visible_metrics.show_disk_write) + ",\n";
    content += SerializeString("network_unit",
                               SerializeNetworkDisplayUnit(config.network_display_unit)) + ",\n";
    content += SerializeString("monitor_popup_mode",
                               SerializePopupActivationMode(config.monitor_popup_mode)) + ",\n";
    content += SerializeString("lord_popup_mode",
                               SerializePopupActivationMode(config.lord_popup_mode)) + ",\n";
    content += SerializeString("toggle_hotkey", SerializeToggleHotkey(config.toggle_hotkey)) + ",\n";
    content += SerializeString("language", SerializeUiLanguage(config.language)) + ",\n";
    content += SerializeString("mode", SerializeAppMode(config.mode)) + ",\n";
    content += SerializeUnsignedInt("sample_interval_seconds",
                                    NormalizeSampleIntervalSeconds(
                                        config.sample_interval_seconds)) + ",\n";
    content += SerializeUnsignedInt("taskbar_monitor_index", config.taskbar_monitor_index) + ",\n";
    content += SerializeBool("auto_start", config.auto_start) + ",\n";
    content += SerializeString("server_host",
                               config.server_host.empty() ? "127.0.0.1" : config.server_host) + ",\n";
    content += SerializeUnsignedInt("server_port",
                                    NormalizeServerPort(config.server_port)) + ",\n";
    content += SerializeString("player_id", config.player_id) + ",\n";
    content += SerializeString("nickname", config.nickname) + "\n";
    content += "}\n";

    HANDLE file_handle = CreateFileW(config_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes_written = 0;
    const BOOL write_ok = WriteFile(file_handle, content.data(),
                                    static_cast<DWORD>(content.size()), &bytes_written, nullptr);
    CloseHandle(file_handle);
    return write_ok != FALSE && bytes_written == content.size();
}

}  // namespace minimal_taskbar_monitor
