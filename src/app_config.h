#pragma once

#include "system_metrics.h"

#include <string>

namespace minimal_taskbar_monitor {

enum class PopupActivationMode {
    kHover,
    kClick,
};

enum class ToggleHotkey {
    kAltQ,
    kAltS,
    kAltM,
    kCtrlAltQ,
};

enum class UiLanguage {
    kEnglish,
    kChinese,
};

// 启动时显示哪一块面板。伪装靠 Alt+Q 切换，但首次启动应该看到游戏本身，
// 否则用户会以为任务栏上只有个性能监视器。
enum class AppMode {
    kMonitor,
    kLord,
};

struct AppConfig {
    MetricVisibility visible_metrics{};
    NetworkDisplayUnit network_display_unit{NetworkDisplayUnit::kBitsPerSecond};
    PopupActivationMode monitor_popup_mode{PopupActivationMode::kHover};
    PopupActivationMode lord_popup_mode{PopupActivationMode::kClick};
    ToggleHotkey toggle_hotkey{ToggleHotkey::kAltQ};
    UiLanguage language{UiLanguage::kChinese};
    AppMode mode{AppMode::kLord};
    unsigned int sample_interval_seconds{1};
    unsigned int taskbar_monitor_index{0};
    bool auto_start{false};

    // 联机：服务器地址与本地身份。
    // 不做账号系统：本地生成一个随机 ID 当作云存档与排行榜的键。
    std::string server_host{"127.0.0.1"};
    unsigned int server_port{8787};
    std::string player_id{};
    std::string nickname{};

};

AppConfig LoadAppConfig();
bool SaveAppConfig(const AppConfig& config);
std::wstring GetAppConfigPath();
std::wstring GetSaveFilePath();

}  // namespace minimal_taskbar_monitor
