#pragma once

#include "Core/Event.h"
#include "Core/KeyShortcut.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <wil/resource.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <windows.h>

namespace Locket
{
    // Low-level keyboard hook (WH_KEYBOARD_LL), installed on a dedicated message-loop thread, used for two things
    // RegisterHotKey cannot do:
    //  * Win-key shortcuts that Windows keeps for itself (Win+V, Win+Shift+C, ...): each shortcut
    //    set with SetShortcuts is swallowed (and the Start menu suppressed on Win key release by
    //    injecting a dummy VK 0xE8 key) and ShortcutPressed is raised instead, so Locket opens
    //    instead of the Windows feature. Shortcuts set with SetBlockedShortcuts are swallowed the
    //    same way without raising anything (Block Win+C).
    //  * Paste detection for the paste stack: PasteDetected is raised when the user presses Ctrl+V
    //    or Shift+Insert anywhere (raised on key up, like Maccy). Injected input (LLKHF_INJECTED,
    //    i.e. Locket's own paste) is ignored.
    // The hook is only installed while at least one feature is enabled. The callback must stay
    // fast; events are posted to the UI thread's queue rather than raised inside the hook.
    class KeyboardHook
    {
    public:
        static KeyboardHook& Instance();

        // Win-key shortcuts to intercept (empty = none). Each must include Modifiers::Win.
        void SetShortcuts(std::vector<KeyShortcut> shortcuts);
        void SetBlockedShortcuts(std::vector<KeyShortcut> shortcuts);
        void EnablePasteDetection(bool enable);
        // Re-check/reinstall the dedicated hook after focus transitions.
        void Refresh();

        Event<> ShortcutPressed;
        Event<> PasteDetected;

    private:
        KeyboardHook() = default;
        ~KeyboardHook();
        void UpdateHook();
        void HookThread();
        void ResetHookState() noexcept;
        static LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam);

        HHOOK m_hook = nullptr;
        mutable std::mutex m_configMutex;
        std::vector<KeyShortcut> m_shortcuts;
        std::vector<KeyShortcut> m_blocked;
        bool m_detectPaste = false;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };
        wil::unique_event m_ready{ wil::EventOptions::ManualReset };
        std::atomic_bool m_stop{ false };
        std::atomic<DWORD> m_threadId{ 0 };
        std::thread m_thread;
    };
}
