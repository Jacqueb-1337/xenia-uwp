#pragma once

#include <string>
#include <vector>
#include <mutex>

namespace UWP {
void ShowKeyboard();
void HandleCharacter(uint32_t keycode);
void HandleVirtualKey(uint32_t keycode, bool down);
bool IsVirtualKeyDown(uint32_t keycode);

extern std::vector<uint32_t> g_char_buffer;
extern std::mutex g_buffer_mutex;
}