#include "pch.h"
#include "ff_util.h"

namespace ff {

namespace {

thread_local std::string g_lastErrorUtf8;
thread_local char g_textReturnBuffer[4096];

std::string WideToMultiByte(UINT code_page, std::wstring_view wide)
{
    if (wide.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(
        code_page, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(
        code_page, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring MultiByteToWide(UINT code_page, std::string_view bytes)
{
    if (bytes.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(
        code_page, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(
        code_page, 0, bytes.data(), static_cast<int>(bytes.size()), out.data(), chars);
    return out;
}

} // namespace

void ClearLastError()
{
    g_lastErrorUtf8.clear();
}

void SetLastError(std::string_view utf8_msg)
{
    g_lastErrorUtf8 = utf8_msg;
}

const char* GetLastErrorGBK()
{
    return CopyToThreadTextBuffer(Utf8ToGbk(g_lastErrorUtf8));
}

std::string Utf8ToGbk(std::string_view utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const std::wstring wide = MultiByteToWide(CP_UTF8, utf8);
    return WideToMultiByte(936, wide);
}

std::string GbkToUtf8(std::string_view gbk)
{
    if (gbk.empty()) {
        return {};
    }
    const std::wstring wide = MultiByteToWide(936, gbk);
    return WideToMultiByte(CP_UTF8, wide);
}

const char* CopyToThreadTextBuffer(const std::string& gbk_text)
{
    const size_t copy_len = (gbk_text.size() < sizeof(g_textReturnBuffer) - 1)
        ? gbk_text.size()
        : sizeof(g_textReturnBuffer) - 1;
    if (copy_len > 0) {
        memcpy(g_textReturnBuffer, gbk_text.data(), copy_len);
    }
    g_textReturnBuffer[copy_len] = '\0';
    return g_textReturnBuffer;
}

int AlignEven(int value)
{
    return (value <= 0) ? 0 : (value & ~1);
}

int64_t BitrateFromKbPerSec(int kb_per_sec)
{
    if (kb_per_sec <= 0) {
        kb_per_sec = 2500;
    }
    return static_cast<int64_t>(kb_per_sec) * 1024 * 8;
}

} // namespace ff
