#include "WinRTKeyboard.h"

#include <winrt/Windows.UI.ViewManagement.Core.h>

#include <array>

namespace UWP {
std::vector<uint32_t> g_char_buffer;
std::mutex g_buffer_mutex;
std::array<bool, 256> g_virtual_key_state = {};

void ShowKeyboard() {
  winrt::Windows::UI::ViewManagement::Core::CoreInputView::GetForCurrentView()
      .TryShowPrimaryView();
}

void HandleCharacter(uint32_t keycode) {
  std::unique_lock lk(g_buffer_mutex);
  g_char_buffer.push_back(keycode);
}

void HandleVirtualKey(uint32_t keycode, bool down) {
  if (keycode >= g_virtual_key_state.size()) return;
  std::unique_lock lk(g_buffer_mutex);
  g_virtual_key_state[keycode] = down;
}

bool IsVirtualKeyDown(uint32_t keycode) {
  if (keycode >= g_virtual_key_state.size()) return false;
  std::unique_lock lk(g_buffer_mutex);
  return g_virtual_key_state[keycode];
}
}  // namespace UWP