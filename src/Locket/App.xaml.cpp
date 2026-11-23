#include "pch.h"
#include "App.xaml.h"

#include "AppController.h"

#include <winrt/Microsoft.Windows.AppNotifications.h>

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::Windows::AppLifecycle;
}

namespace winrt::Locket::implementation
{
    // Read everything out of the activation arguments immediately. For a redirected activation
    // they are a proxy into the second locket.exe, which exits once the redirect completes; touching
    // them later on the UI thread fails with RPC_S_CALL_FAILED (and crashed Locket).
    App::Activation App::Snapshot(AppActivationArguments const& args)
    {
        Activation activation;
        activation.kind = args.Kind();
        if (activation.kind == ExtendedActivationKind::Launch)
        {
            if (auto launch = args.Data().try_as<winrt::Windows::ApplicationModel::Activation::ILaunchActivatedEventArgs>())
            {
                activation.arguments = launch.Arguments();
            }
        }
        else if (activation.kind == ExtendedActivationKind::AppNotification)
        {
            if (auto notification = args.Data().try_as<winrt::Microsoft::Windows::AppNotifications::AppNotificationActivatedEventArgs>())
            {
                auto copy = winrt::single_threaded_map<winrt::hstring, winrt::hstring>();
                for (auto const& pair : notification.Arguments())
                {
                    copy.Insert(pair.Key(), pair.Value());
                }
                activation.notificationArguments = copy;
            }
        }
        return activation;
    }
}

namespace
{
    bool HasArgument(std::wstring const& commandLine, std::wstring_view expected)
    {
        int count = 0;
        LPWSTR* argv = ::CommandLineToArgvW(commandLine.c_str(), &count);
        if (!argv)
        {
            return false;
        }
        bool found = false;
        for (int i = 0; i < count; ++i)
        {
            if (expected == argv[i])
            {
                found = true;
                break;
            }
        }
        ::LocalFree(argv);
        return found;
    }
}

namespace winrt::Locket::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e) {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
        // Locket lives in the notification area: closing the popup or Settings must not end the app.
        DispatcherShutdownMode(winrt::Microsoft::UI::Xaml::DispatcherShutdownMode::OnExplicitShutdown);
    }

    void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        m_dispatcher = Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();

        auto instance = AppInstance::GetCurrent();
        // Subscribe before startup can create work that triggers a redirected activation. Snapshot
        // the initial activation now because redirected argument objects are remote proxies.
        instance.Activated({ this, &App::OnActivated });
        const Activation initialActivation = Snapshot(instance.GetActivatedEventArgs());

        ::Locket::AppController::Current().Start();
        HandleActivation(initialActivation, true);
    }

    void App::OnActivated(IInspectable const&, AppActivationArguments const& args)
    {
        Activation activation;
        try
        {
            activation = Snapshot(args);
        }
        catch (...)
        {
            return; // the redirecting process is already gone; nothing to act on
        }
        m_dispatcher.TryEnqueue([this, activation = std::move(activation)] {
            try
            {
                HandleActivation(activation, false);
            }
            catch (...)
            {
                OutputDebugStringW(L"Locket: exception while handling an activation\n");
            }
        });
    }

    void App::HandleActivation(Activation const& activation, bool initial)
    {
        auto& controller = ::Locket::AppController::Current();
        auto& settings = ::Locket::Settings::Instance();
        std::wstring const& arguments = activation.arguments;

        // A click on one of Locket's notifications while Locket wasn't running (when it runs, the
        // click arrives through AppNotificationManager::NotificationInvoked instead).
        if (activation.kind == ExtendedActivationKind::AppNotification)
        {
            controller.OnNotificationInvoked(activation.notificationArguments);
            return;
        }

        if (HasArgument(arguments, L"--quit"))
        {
            controller.Quit();
            return;
        }
        if (HasArgument(arguments, L"--ignore-next"))
        {
            controller.IgnoreNextCopy();
            return;
        }
        if (HasArgument(arguments, L"--pause"))
        {
            settings.IgnoreOnlyNextEvent(false);
            settings.IgnoreEvents(true);
            return;
        }
        if (HasArgument(arguments, L"--resume"))
        {
            settings.IgnoreOnlyNextEvent(false);
            settings.IgnoreEvents(false);
            return;
        }
        if (HasArgument(arguments, L"--settings"))
        {
            controller.OpenSettings();
            return;
        }

        // Started with Windows, by a scripting command (locket --get ... starts Locket with
        // --background) or after an update (--updated): stay in the notification area.
        if (activation.kind == ExtendedActivationKind::StartupTask || HasArgument(arguments, L"--background") ||
            HasArgument(arguments, L"--updated"))
        {
            return;
        }
        if (initial)
        {
            controller.ShowPopup();
        }
        else
        {
            controller.TogglePopup();
        }
    }
}
