// dllmain.cpp : 定义 DLL 应用程序的入口点。
#include "pch.h"
#include <mutex>

extern "C" {
#include <libavformat/avformat.h>
}

namespace {
std::once_flag g_ffmpeg_init_flag;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        std::call_once(g_ffmpeg_init_flag, [] {
            avformat_network_init();        });
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
