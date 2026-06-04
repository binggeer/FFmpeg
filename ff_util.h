#pragma once

#include <string>
#include <string_view>

namespace ff {

void ClearLastError();
void SetLastError(std::string_view utf8_msg);
const char* GetLastErrorGBK();

std::string Utf8ToGbk(std::string_view utf8);
std::string GbkToUtf8(std::string_view gbk);

const char* CopyToThreadTextBuffer(const std::string& gbk_text);

int AlignEven(int value);
int64_t BitrateFromKbPerSec(int kb_per_sec);

} // namespace ff
