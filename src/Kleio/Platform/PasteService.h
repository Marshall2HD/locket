#pragma once

#include <windows.h>
#include <winrt/Microsoft.UI.Dispatching.h>

namespace Kleio
{
    // Simulates the paste keystroke into the app that was focused before the popup opened.
    class PasteService
    {
    public:
        // Call right before showing the popup: remembers GetForegroundWindow() (ignoring Kleio's
        // own windows).
        void RememberForegroundWindow();
        HWND PreviousForegroundWindow() const noexcept { return m_previous; }
        // Brings the remembered window back to the foreground and returns it only if foreground
        // activation succeeded. No input queues are attached across processes.
        HWND RestoreForegroundWindow();
        // Releases held modifiers, then schedules Ctrl+V after focus settles. The send is aborted
        // if the verified target ceased to be foreground.
        void Paste(HWND target);

    private:
        HWND m_previous = nullptr;
        HWND m_pasteTarget = nullptr;
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_pasteTimer{ nullptr };
    };
}
