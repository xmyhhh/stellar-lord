#pragma once

#include <string>

// 极简内存房间服务器核心。
// 既可被桌面客户端内嵌（主机模式），也可被 stellar_lord_server.exe 独立运行。
namespace rift_server {

// 客户端与服务端共用。任何协议字段或规则不兼容时递增，旧版本会被拒绝联机。
constexpr int kProtocolVersion = 6;

// 同步完成 socket / bind / listen，成功后才返回；错误信息可选。
bool Start(unsigned short port, std::string* error_message = nullptr);
void Stop();
bool IsRunning();
unsigned short Port();

}  // namespace rift_server
