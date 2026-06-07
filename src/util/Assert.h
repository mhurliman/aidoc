#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cassert>
#include <cstdio>

// Fatal assertion with a message — crashes in both Debug and Release builds.
// Uses __debugbreak() to trigger the debugger if attached, then abort().
#define FATAL(msg)                                                                                 \
    do                                                                                             \
    {                                                                                              \
        fprintf(stderr, "FATAL: %s\n  %s(%d)\n", (msg), __FILE__, __LINE__);                       \
        __debugbreak();                                                                            \
        abort();                                                                                   \
    } while (0)

// Assert that an HRESULT succeeded. Crashes with the HRESULT value on failure.
#define ASSERT_SUCCEEDED(hr)                                                                       \
    do                                                                                             \
    {                                                                                              \
        HRESULT hr_ = (hr);                                                                        \
        if (FAILED(hr_))                                                                           \
        {                                                                                          \
            char buf_[256];                                                                        \
            snprintf(buf_, sizeof(buf_), "HRESULT failed: 0x%08X", static_cast<unsigned>(hr_));    \
            FATAL(buf_);                                                                           \
        }                                                                                          \
    } while (0)
