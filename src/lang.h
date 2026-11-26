// UI text: Norwegian when Windows' display language is Norwegian, English otherwise.
#pragma once
#include <windows.h>

inline bool UiNorwegian() {
    static const bool no = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_NORWEGIAN;
    return no;
}
inline const wchar_t* Tr(const wchar_t* en, const wchar_t* no) { return UiNorwegian() ? no : en; }
