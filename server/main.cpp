// Stellar Lord dedicated server.
//
// The HTTP / room logic lives in server_core.cpp so the desktop client can
// embed the exact same server in its "host" launch mode.

#include "server_core.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr unsigned short kDefaultPort = 8787;

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        rift_server::Stop();
        return TRUE;
    }
    return FALSE;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned short port = kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            port = static_cast<unsigned short>(std::atoi(argv[++i]));
        }
    }
    if (port == 0) {
        std::fprintf(stderr, "invalid port: %u\n", static_cast<unsigned int>(port));
        return 1;
    }

    std::string error;
    if (!rift_server::Start(port, &error)) {
        std::fprintf(stderr, "failed to start server: %s\n", error.c_str());
        return 1;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    while (rift_server::IsRunning()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return 0;
}
