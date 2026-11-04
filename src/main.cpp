#include "app_config.h"
#include "background_worker.h"
#include "game_model.h"
#include "net_client.h"
#include "process_monitor.h"
#include "resource.h"
#include "system_metrics.h"
#include "taskbar_embedder.h"

#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
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

constexpr wchar_t kControllerClassName[] = L"OrbitalFreightControllerWindow";
constexpr wchar_t kWidgetClassName[] = L"OrbitalFreightWidgetWindow";
constexpr wchar_t kPopupClassName[] = L"OrbitalFreightPopupWindow";
constexpr wchar_t kServerClassName[] = L"OrbitalFreightServerWindow";
constexpr wchar_t kRoomClassName[] = L"OrbitalFreightRoomWindow";

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
constexpr UINT kModeFreightCommandId = 1004;
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

constexpr UINT kServerSaveCommandId = 2101;
constexpr UINT kServerCancelCommandId = 2102;

constexpr UINT kRoomJoinCommandId = 2201;
constexpr UINT kRoomCancelCommandId = 2202;

constexpr int kServerWindowWidth = 380;
constexpr int kServerWindowHeight = 210;
constexpr int kRoomWindowWidth = 420;
constexpr int kRoomWindowHeight = 235;

constexpr wchar_t kRunRegistryPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"OrbitalFreightTaskbar";

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
// 领主面板数据
// ---------------------------------------------------------------------------

enum class FreightTab {
    kDevelopment = 0,
    kResearch = 1,
    kMilitary = 2,
    kEmpire = 3,
    kOnline = 4,
    kCount = 5,
};

const wchar_t* FreightTabLabel(int index) {
    switch (index) {
    case 0: return T(L"Empire", L"发展");
    case 1: return T(L"Science", L"科研");
    case 2: return T(L"Fleet", L"军事");
    case 3: return T(L"Stars", L"星图");
    case 4:
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
    kColonize,
    kColonyUpgrade,
    kAttackRival,
    kCopyPlayerId,
    kEditServer,
    kRoomCreate,
    kRoomJoinPrompt,
    kRoomJoinCode,
    kRoomLeave,
    kRoomStart,
    kRoomRefresh,
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
};

struct RoomListEntry {
    std::string code;
    std::string host_name;
    int players{0};
    bool started{false};
};

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
    }

    bool Initialize() {
        LoadGame();
        StartSamplers();
        freight_mode_ = app_config_.mode == AppMode::kFreight;
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
        return true;
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
                         FormatCompact(state.minerals) + L"   FOOD " + FormatCompact(state.food);
        freight_line2_ = L"POP " + FormatCompact(state.population) + L"/" +
                         FormatCompact(stellar::PopulationCapacity(state)) + L"   ALY " +
                         FormatCompact(state.alloy) + L"   FLT " + std::to_wstring(state.fleet);
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
        player_name_ = "Lord";
        if (app_config_.player_id.size() >= 4) {
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
        app_config_.mode = freight_mode_ ? AppMode::kFreight : AppMode::kMonitor;
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
                              std::to_wstring(member.colonies);
                    row.right = T(L"Attack", L"进攻");
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
        case FreightTab::kOnline:
        default: {
            FreightRow identity;
            identity.left = T(L"My ID", L"我的 ID");
            identity.mid = Utf8ToWide(app_config_.player_id);
            identity.right = T(L"Copy", L"复制");
            identity.action = RowAction::kCopyPlayerId;
            identity.enabled = true;
            add_row(identity);

            FreightRow server;
            server.left = T(L"Server", L"服务器");
            server.mid = Utf8ToWide(app_config_.server_host) + L":" +
                         std::to_wstring(app_config_.server_port);
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
                    row.left = T(L"Create rift", L"创建时空");
                    row.mid = T(L"become the host, others join with the code",
                                L"成为房主，其他人用房间码加入");
                    row.right = T(L"Create", L"创建");
                    row.action = RowAction::kRoomCreate;
                    row.enabled = true;
                    add_row(row);
                }
                {
                    FreightRow row;
                    row.left = T(L"Join rift", L"加入时空");
                    row.mid = T(L"enter a room code", L"输入房间码");
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
                              std::to_wstring(member.colonies) +
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
                    row.mid = T(L"return to solo play (progress is not saved)",
                                L"回到单机（进度不保存）");
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
        return ScaleByDpi(current_dpi_, freight_mode_ ? 600 : 460);
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
                const int button_width = ScaleByDpi(current_dpi_, 86);
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

            const int right_reserve = ScaleByDpi(current_dpi_, row.has_choice ? 196 : 150);
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
            L"PWR " + FormatCompact(game_.energy) + L"   MIN " + FormatCompact(game_.minerals) +
            L"   FOOD " + FormatCompact(game_.food) + L"   ALY " + FormatCompact(game_.alloy) +
            L"   POP " + FormatCompact(game_.population) + L"   FLT " +
            FormatCompact(static_cast<double>(game_.fleet)) + L"   SCI " +
            FormatCompact(game_.research);
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
        return freight_mode_ ? app_config_.freight_popup_mode : app_config_.monitor_popup_mode;
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

    void ApplyRowAction(const FreightRow& row, bool /*primary*/) {
        switch (row.action) {
        case RowAction::kCopyPlayerId:
            CopyPlayerId();
            return;
        case RowAction::kEditServer:
            ShowServerWindow();
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
            case RowAction::kColonize:
                PostRoomAction("colonize", {});
                return;
            case RowAction::kColonyUpgrade:
                PostRoomAction("colony", std::to_string(row.index));
                return;
            case RowAction::kAttackRival:
                if (row.index >= 0 && row.index < static_cast<int>(room_members_.size())) {
                    PostRoomAction("attack_player",
                                   room_members_[static_cast<std::size_t>(row.index)].id);
                }
                return;
            default:
                return;
            }
        }

        bool handled = true;
        switch (row.action) {
        case RowAction::kBuild:
            stellar::Build(game_, static_cast<stellar::BuildingId>(row.index));
            break;
        case RowAction::kCapital:
            stellar::UpgradeCapital(game_);
            break;
        case RowAction::kResearchTech:
            stellar::BuyTech(game_, static_cast<stellar::TechId>(row.index));
            break;
        case RowAction::kBuildFleet:
            stellar::BuildFleet(game_, row.index);
            break;
        case RowAction::kAttackNeutral: {
            const stellar::BattleResult result = stellar::AttackNeutral(game_);
            ReportBattle(result);
            break;
        }
        case RowAction::kColonize:
            stellar::Colonize(game_);
            break;
        case RowAction::kColonyUpgrade:
            stellar::UpgradeColony(game_, static_cast<std::size_t>(row.index));
            break;
        default:
            handled = false;
            break;
        }
        if (!handled) {
            return;
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
            T(L"Point the client at the freight_server address.",
              L"填写 freight_server 所在机器地址，本机可留 127.0.0.1。"),
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
        return T(L"request failed", L"请求失败");
    }

    std::string PlayerName() const {
        return player_name_;
    }

    void DoRoomCreate() {
        SetNetStatus(T(L"creating rift...", L"正在创建时空…"));
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&name=" + UrlEncode(player_name_);
        PostNetTask(NetTaskKind::kRoomCreate, "/api/room/create", form, true);
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
        room_members_.clear();
        room_poll_pending_ = false;
        room_list_.clear();
    }

    void DoRoomLeave() {
        if (!room_active_) {
            return;
        }
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&room=" + UrlEncode(room_code_);
        PostNetTask(NetTaskKind::kRoomLeave, "/api/room/leave", form, true);
        ClearRoomUi();
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

    void DoRoomStart() {
        if (!room_active_) {
            return;
        }
        SetNetStatus(T(L"starting...", L"正在开始…"));
        std::string form = "id=" + UrlEncode(app_config_.player_id) +
                           "&room=" + UrlEncode(room_code_);
        PostNetTask(NetTaskKind::kRoomStart, "/api/room/start", form, true);
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

    void ApplyRoomEntry(const std::string& body) {
        room_active_ = true;
        room_code_ = NetField(body, "room");
        room_started_ = NetField(body, "started") == "1";
        room_host_ = NetField(body, "host") == "1";
        room_host_id_ = NetField(body, "host_id");
        room_host_name_ = NetField(body, "host_name");
        room_poll_pending_ = false;
        net_status_ = T(L"in rift ", L"已进入时空 ") + Utf8ToWide(room_code_);
        RequestRoomState();
        ShowPopup(true);
        RefreshOnlineUi();
    }

    // -- 加入时空窗口 ---------------------------------------------------------

    void ShowRoomWindow(const std::wstring& code) {
        if (room_window_ != nullptr && IsWindow(room_window_)) {
            SetWindowTextW(room_code_edit_, code.c_str());
            SetWindowTextW(room_name_edit_, Utf8ToWide(player_name_).c_str());
            ShowWindow(room_window_, SW_SHOWNORMAL);
            SetForegroundWindow(room_window_);
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
        SetFocus(room_code_edit_);
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
        make_label(T(L"Rift code", L"房间码"), pad, y, label_width, row_height);
        room_code_edit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_UPPERCASE |
                                                  WS_TABSTOP,
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
            T(L"Ask the host for the 4-letter rift code.",
              L"向房主索取 4 位房间码；所有人以平民身份同时开局。"),
            pad, y, inner_width, std::max(row_height, button_y - y - gap));

        CreateWindowExW(0, L"BUTTON", T(L"Join", L"加入"),
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width * 2 - gap, button_y,
                        button_width, button_height, window_handle,
                        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kRoomJoinCommandId)),
                        instance_handle_, nullptr);
        CreateWindowExW(0, L"BUTTON", T(L"Cancel", L"取消"),
                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                        (client.right - client.left) - pad - button_width, button_y, button_width,
                        button_height, window_handle,
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
        wchar_t code_buffer[32]{};
        wchar_t name_buffer[64]{};
        GetWindowTextW(room_code_edit_, code_buffer, static_cast<int>(std::size(code_buffer)));
        GetWindowTextW(room_name_edit_, name_buffer, static_cast<int>(std::size(name_buffer)));

        std::wstring code = TrimWide(code_buffer);
        std::wstring name = TrimWide(name_buffer);
        std::transform(code.begin(), code.end(), code.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(towupper(ch));
        });
        if (code.size() < 4 || code.size() > 12) {
            SetRoomStatus(T(L"Rift codes are 4-12 characters.", L"房间码为 4-12 个字符。"));
            return;
        }
        for (wchar_t ch : code) {
            if (!iswalnum(ch)) {
                SetRoomStatus(T(L"Rift codes use letters and digits only.",
                                L"房间码只能包含字母和数字。"));
                return;
            }
        }
        if (name.empty() || name.size() > 16) {
            SetRoomStatus(T(L"Nickname must be 1-16 characters.", L"昵称需为 1-16 个字符。"));
            return;
        }

        player_name_ = WideToUtf8(name);
        {
            const std::string trace = "submit code=[" + WideToUtf8(code) + "] name=[" +
                                      player_name_ + "] code_hwnd=" +
                                      std::to_string(reinterpret_cast<INT_PTR>(room_code_edit_)) +
                                      " name_hwnd=" +
                                      std::to_string(reinterpret_cast<INT_PTR>(room_name_edit_)) + "\n";
            HANDLE handle = CreateFileW(L"C:\\Users\\xmyci\\AppData\\Local\\Temp\\opencode\\join_trace.txt",
                                        GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(handle, trace.data(), static_cast<DWORD>(trace.size()), &written,
                          nullptr);
                CloseHandle(handle);
            }
        }
        HideRoomWindow();
        DoRoomJoin(WideToUtf8(code));
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
        RefreshOnlineUi();
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
            SetNetStatus(reason);
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
                ClearRoomUi();
                net_status_ = T(L"the rift was closed", L"时空已关闭");
                game_ = stellar::MakeNewGame(static_cast<double>(std::time(nullptr)));
                last_tick_ms_ = GetTickCount64();
                RefreshOnlineUi();
            } else {
                SetNetStatus(NetErrorText(err));
            }
            return;
        }
        case NetTaskKind::kRoomCreate:
        case NetTaskKind::kRoomJoin: {
            if (net.status == 200 && ok == "1") {
                ApplyRoomEntry(net.body);
            } else {
                SetNetStatus(NetErrorText(err));
            }
            return;
        }
        case NetTaskKind::kRoomLeave:
            return;
        case NetTaskKind::kRoomStart: {
            if (net.status == 200 && ok == "1") {
                net_status_ = T(L"game started", L"对局已开始");
                RequestRoomState();
            } else {
                SetNetStatus(NetErrorText(err));
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
                    ReportBattle(battle);
                } else {
                    net_status_ = T(L"done", L"已完成");
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
        AppendMenuW(mode_menu, MF_STRING | (freight_mode_ ? MF_CHECKED : 0), kModeFreightCommandId,
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
                   L"Empire panel: 1-5 switch tabs, up/down select, Enter confirms, Esc closes.\n"
                   L"Loop: resources -> population -> research -> fleet -> colonize -> capture -> develop.\n"
                   L"Rift tab: create a room and share the code, or join someone else's room.\n"
                   L"Everyone starts as a commoner; the host starts the match for all players.\n"
                   L"Room progress lives only in server memory and is never written to disk.\n"
                   L"Right-click the widget for mode, popup style, hotkey and language.";
        } else {
            text = L"外观是系统监视器，任务栏上的控件其实是游戏。\n"
                   L"按 " + hotkey + L" 在「监视器面板」和「领主面板」之间切换。\n"
                   L"鼠标悬停或点击任务栏控件即可打开面板。\n\n"
                   L"领主面板：1-5 切换标签，上下键选择，回车确认，Esc 关闭。\n"
                   L"核心循环：资源 → 人口 → 科研 → 舰队 → 殖民 → 占领 → 开发。\n"
                   L"联机页（加入时空）：创建房间并把 4 位房间码发给朋友，\n"
                   L"或输入别人的房间码加入；创建者就是房主，由房主统一开始。\n"
                   L"所有人以平民身份同时开局，进度只存在服务端内存里，不写本机文件。\n"
                   L"右键控件可切换模式、弹窗方式、快捷键与语言。";
        }
        MessageBoxW(controller_window_, text.c_str(),
                    T(L"Stellar Lord", L"星际领主"), MB_OK | MB_ICONINFORMATION);
    }

    bool SetPopupMode(PopupActivationMode mode) {
        if (freight_mode_) {
            app_config_.freight_popup_mode = mode;
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
        case kModeFreightCommandId:
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
    std::vector<RoomMemberInfo> room_members_{};
    std::vector<RoomListEntry> room_list_{};
    ULONGLONG last_room_poll_ms_{0};

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
