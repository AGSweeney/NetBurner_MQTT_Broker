// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
// SPDX-License-Identifier: MIT

// Redirect MSVC debug CRT asserts/errors to stderr (no Windows popup dialogs).
// Linked into every host test executable via mqtt_test_support.

#include "test_host_env.hpp"

#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <crtdbg.h>
#include <csignal>
#include <windows.h>
#endif

namespace mqtt_broker {
namespace {

#ifdef _WIN32
bool g_host_env_ready = false;

int headless_report_hook(int report_type, char *message, int *return_value)
{
    (void)report_type;
    if (message != nullptr) {
        fputs(message, stderr);
        fflush(stderr);
    }
    *return_value = 0;  // do not break into the debugger
    return TRUE;         // handled: suppress the modal dialog
}

void redirect_crt_reports()
{
    _set_error_mode(_OUT_TO_STDERR);

    // Keep abort()/assert text on stderr; suppress only the modal fault UI.
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    _CrtSetReportHook(headless_report_hook);

    const int modes = _CRTDBG_MODE_FILE;
    const _HFILE sink = _CRTDBG_FILE_STDERR;

    _CrtSetReportMode(_CRT_ERROR, modes);
    _CrtSetReportFile(_CRT_ERROR, sink);
    _CrtSetReportMode(_CRT_ASSERT, modes);
    _CrtSetReportFile(_CRT_ASSERT, sink);
    _CrtSetReportMode(_CRT_WARN, modes);
    _CrtSetReportFile(_CRT_WARN, sink);

    signal(SIGABRT, [](int) { _exit(3); });
}
#endif

struct HostTestEnv {
    HostTestEnv()
    {
#ifdef _WIN32
        if (!g_host_env_ready) {
            redirect_crt_reports();
            g_host_env_ready = true;
        }
#endif
    }
};

const HostTestEnv g_host_test_env{};

}  // namespace

void init_test_host_env()
{
#ifdef _WIN32
    if (!g_host_env_ready) {
        redirect_crt_reports();
        g_host_env_ready = true;
    }
#endif
}

}  // namespace mqtt_broker
