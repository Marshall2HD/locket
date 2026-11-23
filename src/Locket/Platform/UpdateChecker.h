#pragma once

#include "Core/Event.h"

#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <functional>
#include <string>

namespace Locket
{
    // GitHub builds query the latest Marshall2HD/locket release.  The URL exposed to the UI is
    // always derived from a validated tag_name, never from a release-supplied URL.
    struct UpdateInfo
    {
        std::wstring version;     // "1.2.3"
        std::wstring releaseUrl;  // validated https://github.com/Marshall2HD/locket/releases/tag/vX.Y.Z
    };

    enum class UpdateCheckStatus
    {
        UpToDate,
        Available,
        NoRelease,
        Failed,
    };

    struct UpdateCheckResult
    {
        UpdateCheckStatus status = UpdateCheckStatus::Failed;
        UpdateInfo update;
    };

    inline constexpr std::chrono::seconds UpdateCheckInterval{ 24 * 60 * 60 };
    inline constexpr wchar_t ReleasesUrl[] = L"https://github.com/Marshall2HD/locket/releases/latest";

    // Never throws. `done` is called on the calling UI thread when one exists.
    winrt::fire_and_forget CheckForUpdate(std::function<void(UpdateCheckResult const&)> done);

    // Opens the validated GitHub release page. `ok` is true only when launching the browser was
    // successfully requested; callers must show failure UI only when it is false.
    winrt::fire_and_forget InstallUpdate(UpdateInfo update, std::function<void(bool ok, std::wstring const& error)> done);

    class UpdateScheduler
    {
    public:
        static UpdateScheduler& Instance();
        void Start();
        void Stop();
        Event<UpdateInfo> UpdateFound;

    private:
        UpdateScheduler() = default;
        void Tick();
        winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer m_timer{ nullptr };
        bool m_checking = false;
    };

    double UnixNow() noexcept;
}
