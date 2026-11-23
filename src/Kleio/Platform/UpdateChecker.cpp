#include "pch.h"
#include "Platform/UpdateChecker.h"

#ifndef KLEIO_STORE_BUILD
#include "Platform/SystemIntegration.h"
#include "Core/Settings.h"
#endif

namespace Kleio
{
    double UnixNow() noexcept
    {
        using namespace std::chrono;
        return duration_cast<duration<double>>(system_clock::now().time_since_epoch()).count();
    }

#ifdef KLEIO_STORE_BUILD
    // Store packages must not link an alternate update path.  Keep these deliberately inert: no
    // HTTP, deployment, restart, shell, or package-management APIs are referenced in this build.
    winrt::fire_and_forget CheckForUpdate(std::function<void(UpdateCheckResult const&)> done)
    {
        if (done)
        {
            done(UpdateCheckResult{});
        }
        co_return;
    }

    winrt::fire_and_forget InstallUpdate(UpdateInfo, std::function<void(bool, std::wstring const&)> done)
    {
        if (done)
        {
            done(false, L"");
        }
        co_return;
    }

    UpdateScheduler& UpdateScheduler::Instance()
    {
        static UpdateScheduler instance;
        return instance;
    }

    void UpdateScheduler::Start() {}
    void UpdateScheduler::Stop() {}
    void UpdateScheduler::Tick() {}
#else
    namespace
    {
        constexpr wchar_t LatestReleaseApi[] = L"https://api.github.com/repos/Marshall2HD/locket/releases/latest";
        constexpr wchar_t ReleaseTagPrefix[] = L"https://github.com/Marshall2HD/locket/releases/tag/";

        void Log(std::wstring_view message)
        {
            std::wstring line{ L"[Kleio] updates: " };
            line.append(message);
            line.push_back(L'\n');
            ::OutputDebugStringW(line.c_str());
        }

        std::wstring NormalizeVersion(std::wstring version)
        {
            const auto first = version.find_first_not_of(L" \t\r\n");
            version.erase(0, first == std::wstring::npos ? version.size() : first);
            if (!version.empty() && (version.front() == L'v' || version.front() == L'V'))
            {
                version.erase(0, 1);
            }
            const auto suffix = version.find_first_of(L"-+ \t\r\n");
            if (suffix != std::wstring::npos)
            {
                version.resize(suffix);
            }
            return version;
        }

        std::vector<std::uint64_t> ParseVersion(std::wstring const& version)
        {
            std::vector<std::uint64_t> parts;
            std::uint64_t current = 0;
            bool any = false;
            for (wchar_t c : version)
            {
                if (c == L'.')
                {
                    if (!any)
                    {
                        return {};
                    }
                    parts.push_back(current);
                    current = 0;
                    any = false;
                }
                else if (c >= L'0' && c <= L'9')
                {
                    current = current * 10 + static_cast<std::uint64_t>(c - L'0');
                    any = true;
                    if (current > 0xFFFFFFFFull)
                    {
                        return {};
                    }
                }
                else
                {
                    return {};
                }
            }
            if (!any)
            {
                return {};
            }
            parts.push_back(current);
            return parts;
        }

        int CompareVersions(std::vector<std::uint64_t> const& a, std::vector<std::uint64_t> const& b)
        {
            const std::size_t count = (std::max)(a.size(), b.size());
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto left = i < a.size() ? a[i] : 0;
                const auto right = i < b.size() ? b[i] : 0;
                if (left != right)
                {
                    return left < right ? -1 : 1;
                }
            }
            return 0;
        }

        // Only plain vX.Y.Z-style tags are accepted. This both validates the version comparison
        // and makes the derived release URL safe without trusting html_url/browser_download_url.
        std::optional<UpdateInfo> UpdateFromTag(std::wstring const& tag)
        {
            if (tag.size() < 2 || (tag.front() != L'v' && tag.front() != L'V'))
            {
                return std::nullopt;
            }
            const std::wstring version = NormalizeVersion(tag);
            if (ParseVersion(version).empty() || tag != L"v" + version)
            {
                return std::nullopt;
            }
            return UpdateInfo{ version, std::wstring{ ReleaseTagPrefix } + tag };
        }

        winrt::Windows::Web::Http::HttpClient MakeClient()
        {
            winrt::Windows::Web::Http::HttpClient client;
            auto headers = client.DefaultRequestHeaders();
            headers.UserAgent().TryParseAdd(L"Locket");
            headers.Accept().TryParseAdd(L"application/vnd.github+json");
            return client;
        }
    }

    winrt::fire_and_forget CheckForUpdate(std::function<void(UpdateCheckResult const&)> done)
    {
        auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        UpdateCheckResult result;
        if (SystemIntegration::InstalledFromStore())
        {
            if (done)
            {
                done(result);
            }
            co_return;
        }
        try
        {
            const auto current = ParseVersion(NormalizeVersion(std::wstring{ SystemIntegration::AppVersion() }));
            co_await winrt::resume_background();
            auto client = MakeClient();
            const auto response = co_await client.GetAsync(winrt::Windows::Foundation::Uri{ LatestReleaseApi });
            if (response.StatusCode() == winrt::Windows::Web::Http::HttpStatusCode::NotFound)
            {
                result.status = UpdateCheckStatus::NoRelease;
            }
            else if (response.IsSuccessStatusCode())
            {
                winrt::Windows::Data::Json::JsonObject json{ nullptr };
                const auto body = co_await response.Content().ReadAsStringAsync();
                if (winrt::Windows::Data::Json::JsonObject::TryParse(body, json) && json)
                {
                    if (auto update = UpdateFromTag(std::wstring{ json.GetNamedString(L"tag_name", L"") }))
                    {
                        result.status = CompareVersions(ParseVersion(update->version), current) > 0
                            ? UpdateCheckStatus::Available : UpdateCheckStatus::UpToDate;
                        if (result.status == UpdateCheckStatus::Available)
                        {
                            result.update = std::move(*update);
                        }
                    }
                }
            }
            else
            {
                Log(L"GitHub answered HTTP " + std::to_wstring(static_cast<int>(response.StatusCode())));
            }
        }
        catch (...)
        {
            result = {};
        }
        if (dispatcher)
        {
            co_await wil::resume_foreground(dispatcher);
        }
        try { if (done) done(result); } catch (...) {}
    }

    winrt::fire_and_forget InstallUpdate(UpdateInfo update, std::function<void(bool, std::wstring const&)> done)
    {
        bool ok = false;
        std::wstring error;
        if (!SystemIntegration::InstalledFromStore() && update.releaseUrl.starts_with(ReleaseTagPrefix))
        {
            try
            {
                auto operation = winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri{ update.releaseUrl });
                ok = co_await operation;
            }
            catch (winrt::hresult_error const& e)
            {
                error = e.message();
            }
            catch (...)
            {
                error = L"Unable to open the release page.";
            }
        }
        if (!ok && error.empty())
        {
            error = L"Unable to open the release page.";
        }
        try { if (done) done(ok, error); } catch (...) {}
    }

    UpdateScheduler& UpdateScheduler::Instance()
    {
        static UpdateScheduler instance;
        return instance;
    }

    void UpdateScheduler::Start()
    {
        if (SystemIntegration::InstalledFromStore())
        {
            return;
        }
        if (!m_timer)
        {
            auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
            if (!dispatcher) return;
            m_timer = dispatcher.CreateTimer();
            m_timer.IsRepeating(true);
            m_timer.Tick([this](auto&&, auto&&) { Tick(); });
        }
        m_timer.Stop();
        m_timer.Interval(std::chrono::seconds(10));
        m_timer.Start();
    }

    void UpdateScheduler::Stop()
    {
        if (m_timer) m_timer.Stop();
    }

    void UpdateScheduler::Tick()
    {
        if (SystemIntegration::InstalledFromStore()) return;
        if (m_timer && m_timer.Interval() != std::chrono::hours(1))
        {
            m_timer.Stop();
            m_timer.Interval(std::chrono::hours(1));
            m_timer.Start();
        }
        auto& settings = Settings::Instance();
        if (!settings.CheckForUpdates() || m_checking) return;
        const double now = UnixNow();
        if (const double last = settings.LastUpdateCheck(); last <= now && now - last < static_cast<double>(UpdateCheckInterval.count())) return;
        m_checking = true;
        settings.LastUpdateCheck(now);
        CheckForUpdate([this](UpdateCheckResult const& result) {
            m_checking = false;
            if (result.status == UpdateCheckStatus::Available && result.update.version != Settings::Instance().SkippedUpdateVersion())
            {
                UpdateFound(result.update);
            }
        });
    }
#endif
}
