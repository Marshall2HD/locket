#include "pch.h"
#include "Platform/PasteService.h"

#pragma comment(lib, "user32.lib")

namespace Locket
{
    namespace
    {
        // Unassigned virtual key; injecting it between a modifier's down and up keeps Windows from
        // treating a lone Alt/Win press as "open the menu bar" / "open Start".
        constexpr WORD DummyKey = 0xE8;

        bool IsOwnWindow(HWND hwnd) noexcept
        {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            return pid == GetCurrentProcessId();
        }

        INPUT KeyInput(WORD vk, bool up) noexcept
        {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = vk;
            input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            switch (vk)
            {
            case VK_RCONTROL:
            case VK_RMENU:
            case VK_LWIN:
            case VK_RWIN:
            case VK_INSERT:
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                break;
            default:
                break;
            }
            return input;
        }

        bool IsDown(int vk) noexcept
        {
            return (GetAsyncKeyState(vk) & 0x8000) != 0;
        }

        bool TrySetForeground(HWND hwnd) noexcept
        {
            SetForegroundWindow(hwnd);
            return GetForegroundWindow() == hwnd;
        }
    }

    void PasteService::RememberForegroundWindow()
    {
        try
        {
            const HWND foreground = GetForegroundWindow();
            if (foreground && !IsOwnWindow(foreground))
            {
                m_previous = foreground;
            }
        }
        catch (...)
        {
        }
    }

    HWND PasteService::RestoreForegroundWindow()
    {
        try
        {
            const HWND target = m_previous;
            if (!target || !IsWindow(target))
            {
                return nullptr;
            }
            if (IsIconic(target))
            {
                ShowWindow(target, SW_RESTORE);
            }
            if (TrySetForeground(target))
            {
                return target;
            }

            // A zero mouse input makes this process the one that received the last input event,
            // which can lift the foreground lock without attaching two processes' input queues.
            INPUT nudge{};
            nudge.type = INPUT_MOUSE;
            SendInput(1, &nudge, sizeof(INPUT));
            if (TrySetForeground(target))
            {
                return target;
            }

            // Last resort: tap Alt (with a dummy key so the target doesn't enter menu mode).
            INPUT altDown[] = { KeyInput(VK_MENU, false) };
            SendInput(1, altDown, sizeof(INPUT));
            SetForegroundWindow(target);
            INPUT altUp[] = { KeyInput(DummyKey, false), KeyInput(DummyKey, true), KeyInput(VK_MENU, true) };
            SendInput(static_cast<UINT>(std::size(altUp)), altUp, sizeof(INPUT));
            return GetForegroundWindow() == target ? target : nullptr;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    void PasteService::Paste(HWND target)
    {
        try
        {
            if (!target || !IsWindow(target))
            {
                return;
            }
            // Release modifiers the user may still be holding (e.g. Alt from Alt+Enter), otherwise the
            // target would see Ctrl+Alt+V etc.
            std::vector<INPUT> release;
            const bool altOrWinHeld = IsDown(VK_LMENU) || IsDown(VK_RMENU) || IsDown(VK_LWIN) || IsDown(VK_RWIN);
            if (altOrWinHeld)
            {
                release.push_back(KeyInput(DummyKey, false));
                release.push_back(KeyInput(DummyKey, true));
            }
            static constexpr WORD kModifierKeys[] = { VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN };
            for (WORD vk : kModifierKeys)
            {
                if (IsDown(vk)) release.push_back(KeyInput(vk, true));
            }
            if (!release.empty()) SendInput(static_cast<UINT>(release.size()), release.data(), sizeof(INPUT));

            if (!m_pasteTimer)
            {
                auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
                if (!dispatcher) return;
                m_pasteTimer = dispatcher.CreateTimer();
                m_pasteTimer.IsRepeating(false);
                m_pasteTimer.Tick([this](winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer const& timer, auto&&) {
                    timer.Stop();
                    const HWND target = m_pasteTarget;
                    m_pasteTarget = nullptr;
                    if (!IsWindow(target) || GetForegroundWindow() != target)
                    {
                        return; // focus moved or the target is elevated
                    }
                    const HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(target, nullptr));
                    const SHORT scan = VkKeyScanExW(L'v', layout);
                    const WORD vKey = scan != -1 && LOBYTE(scan) != 0 ? LOBYTE(scan) : 'V';
                    INPUT inputs[] = { KeyInput(VK_CONTROL, false), KeyInput(vKey, false), KeyInput(vKey, true), KeyInput(VK_CONTROL, true) };
                    if (SendInput(static_cast<UINT>(std::size(inputs)), inputs, sizeof(INPUT)) != std::size(inputs))
                    {
                        OutputDebugStringW(L"Locket: SendInput(Ctrl+V) was blocked\n");
                    }
                });
            }
            m_pasteTarget = target;
            m_pasteTimer.Stop();
            m_pasteTimer.Interval(std::chrono::milliseconds(50));
            m_pasteTimer.Start();
        }
        catch (...)
        {
        }
    }
}
