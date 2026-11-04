#pragma once

// 联机网络层：极简 HTTP/1.1 客户端（Winsock2，无第三方依赖）。
// 所有函数都是阻塞的，调用方负责放到后台线程里执行。

#include <string>

namespace minimal_taskbar_monitor {

struct NetResponse {
    bool transport_ok{false};  // 是否拿到了一个合法的 HTTP 响应
    int status{0};             // HTTP 状态码
    std::string body;          // 响应体
    std::string error;         // 传输层失败原因：dns / connect / send / recv
};

// 发送 GET 请求。path 可以带查询串，例如 "/api/save?token=abc"。
NetResponse HttpGet(const std::string& host, unsigned short port, const std::string& path,
                    int timeout_ms = 5000);

// 发送 application/x-www-form-urlencoded POST 请求。
NetResponse HttpPostForm(const std::string& host, unsigned short port, const std::string& path,
                         const std::string& form_body, int timeout_ms = 8000);

std::string UrlEncode(const std::string& text);

std::string WideToUtf8(const std::wstring& text);
std::wstring Utf8ToWide(const std::string& text);

}  // namespace minimal_taskbar_monitor
