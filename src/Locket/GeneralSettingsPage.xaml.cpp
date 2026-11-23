#include "pch.h"
#include "GeneralSettingsPage.xaml.h"
#if __has_include("GeneralSettingsPage.g.cpp")
#include "GeneralSettingsPage.g.cpp"
#endif

#include "AppController.h"
#include "Core/ItemAction.h"
#include "Core/Settings.h"
#include "Platform/SystemIntegration.h"
#include "SettingsHelpers.h"
#include "Strings.h"

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
    using namespace Microsoft::UI::Xaml::Controls;
}

namespace winrt::Locket::implementation
{
    GeneralSettingsPage::~GeneralSettingsPage()
    {
        Unsubscribe();
    }

    void GeneralSettingsPage::InitializeComponent()
    {
        GeneralSettingsPageT<GeneralSettingsPage>::InitializeComponent();

        PopupRecorder().RequireModifier(true);
        PinRecorder().RequireModifier(true);
        DeleteRecorder().RequireModifier(true);
        PreviewRecorder().RequireModifier(true);

        auto handler = [weak = get_weak()](winrt::Locket::ShortcutRecorder const& sender, winrt::Windows::Foundation::IInspectable const& args) {
            if (auto self = weak.get())
            {
                self->OnShortcutChanged(sender, args);
            }
        };
        PopupRecorder().ShortcutChanged(handler);
        PinRecorder().ShortcutChanged(handler);
        DeleteRecorder().ShortcutChanged(handler);
        PreviewRecorder().ShortcutChanged(handler);

        if (::Locket::SystemIntegration::InstalledFromStore())
        {
            CheckForUpdatesCard().Visibility(winrt::Visibility::Collapsed);
        }

        // "Windows default", then every language Locket is translated into, by its own name.
        auto items = LanguageCombo().Items();
        items.Append(winrt::box_value(winrt::hstring{ ::Locket::Strings::Get(L"General_LanguageDefault") }));
        for (auto const& language : ::Locket::Strings::Languages())
        {
            winrt::ComboBoxItem item;
            item.Content(winrt::box_value(winrt::hstring{ language.name }));
            item.Language(winrt::hstring{ language.tag });
            items.Append(item);
        }
    }

    void GeneralSettingsPage::Page_Loaded(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            LoadValues();
            LoadShortcuts();
            LoadLaunchAtLoginAsync();

            if (!m_settingsToken)
            {
                auto weak = get_weak();
                m_settingsToken = ::Locket::Settings::Instance().Changed.Add([weak](std::wstring_view key) {
                    if (auto self = weak.get())
                    {
                        self->OnSettingChanged(key);
                    }
                });
            }
        }
        catch (...)
        {
        }
    }

    void GeneralSettingsPage::Page_Unloaded(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        Unsubscribe();
    }

    void GeneralSettingsPage::Unsubscribe() noexcept
    {
        if (m_settingsToken)
        {
            ::Locket::Settings::Instance().Changed.Remove(*m_settingsToken);
            m_settingsToken.reset();
        }
    }

    void GeneralSettingsPage::OnSettingChanged(std::wstring_view key)
    {
        if (m_loading)
        {
            return;
        }
        // Shortcuts are only edited here (and are temporarily cleared while a recorder records),
        // so they are not reloaded from change notifications.
        using S = ::Locket::Settings;
        if (key == S::kPopupShortcut || key == S::kPinShortcut || key == S::kDeleteShortcut || key == S::kTogglePreviewShortcut)
        {
            return;
        }
        try
        {
            LoadValues();
        }
        catch (...)
        {
        }
    }

    void GeneralSettingsPage::LoadValues()
    {
        auto& settings = ::Locket::Settings::Instance();
        m_loading = true;
        try
        {
            CheckForUpdatesToggle().IsOn(settings.CheckForUpdates());
            int32_t languageIndex = 0;
            auto const languages = ::Locket::Strings::Languages();
            for (std::size_t i = 0; i < languages.size(); ++i)
            {
                if (languages[i].tag == settings.Language())
                {
                    languageIndex = static_cast<int32_t>(i + 1);
                }
            }
            LanguageCombo().SelectedIndex(languageIndex);
            LanguageInfoBar().IsOpen(settings.Language() != ::Locket::Strings::InitialLanguage());
            SearchModeCombo().SelectedIndex(static_cast<int32_t>(settings.SearchMode()));
            PasteAutomaticallyToggle().IsOn(settings.PasteByDefault());
            RemoveFormattingToggle().IsOn(settings.RemoveFormattingByDefault());
            ReplaceWinVToggle().IsOn(settings.ReplaceWinV());
            BlockWinCToggle().IsOn(settings.BlockWinC());
            PlaySoundsToggle().IsOn(settings.PlaySounds());
            ShowNotificationsToggle().IsOn(settings.ShowNotifications());
            NotificationsInfoBar().IsOpen(settings.ShowNotifications() && !::Locket::SystemIntegration::AreAppNotificationsEnabled());
            RefreshModifiers();
        }
        catch (...)
        {
        }
        m_loading = false;
    }

    void GeneralSettingsPage::LoadShortcuts()
    {
        auto& settings = ::Locket::Settings::Instance();
        if (!PopupRecorder().IsRecording())
        {
            PopupRecorder().Shortcut(winrt::hstring{ settings.PopupShortcut().ToString() });
        }
        PinRecorder().Shortcut(winrt::hstring{ settings.PinShortcut().ToString() });
        DeleteRecorder().Shortcut(winrt::hstring{ settings.DeleteShortcut().ToString() });
        PreviewRecorder().Shortcut(winrt::hstring{ settings.TogglePreviewShortcut().ToString() });
    }

    // Maccy's GeneralSettingsPane.refreshModifiers.
    void GeneralSettingsPage::RefreshModifiers()
    {
        auto& settings = ::Locket::Settings::Instance();
        bool const paste = settings.PasteByDefault();
        bool const plain = settings.RemoveFormattingByDefault();
        using ::Locket::ItemAction;
        auto const copyText = ::Locket::SettingsUi::FormatModifiers(::Locket::ModifiersForAction(ItemAction::Copy, paste, plain));
        auto const pasteText = ::Locket::SettingsUi::FormatModifiers(::Locket::ModifiersForAction(ItemAction::Paste, paste, plain));
        auto const plainText = ::Locket::SettingsUi::FormatModifiers(::Locket::ModifiersForAction(ItemAction::PasteWithoutFormatting, paste, plain));

        ModifiersDescription().Text(::Locket::Strings::Format(L"General_Modifiers", copyText, pasteText, plainText));
    }

    winrt::fire_and_forget GeneralSettingsPage::LoadLaunchAtLoginAsync()
    {
        auto strong = get_strong();
        auto dispatcher = this->DispatcherQueue();
        bool enabled = false;
        try
        {
            enabled = co_await ::Locket::SystemIntegration::IsLaunchAtLoginEnabled();
        }
        catch (...)
        {
        }
        co_await wil::resume_foreground(dispatcher);
        m_loading = true;
        try
        {
            LaunchAtLoginToggle().IsOn(enabled);
        }
        catch (...)
        {
        }
        m_loading = false;
    }

    winrt::fire_and_forget GeneralSettingsPage::SetLaunchAtLoginAsync(bool enable)
    {
        auto strong = get_strong();
        auto dispatcher = this->DispatcherQueue();
        bool result = !enable;
        try
        {
            LaunchAtLoginToggle().IsEnabled(false);
            result = co_await ::Locket::SystemIntegration::SetLaunchAtLoginEnabled(enable);
        }
        catch (...)
        {
        }
        co_await wil::resume_foreground(dispatcher);
        m_loading = true;
        try
        {
            LaunchAtLoginToggle().IsEnabled(true);
            LaunchAtLoginToggle().IsOn(result);
            LaunchAtLoginInfoBar().IsOpen(enable && !result);
        }
        catch (...)
        {
        }
        m_loading = false;
    }

    void GeneralSettingsPage::LaunchAtLoginToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        SetLaunchAtLoginAsync(LaunchAtLoginToggle().IsOn());
    }

    void GeneralSettingsPage::CheckForUpdatesToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().CheckForUpdates(CheckForUpdatesToggle().IsOn());
    }

    void GeneralSettingsPage::CheckNowButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        ::Locket::SettingsUi::CheckForUpdates(UpdateInfoBar(), CheckNowButton());
    }

    void GeneralSettingsPage::SearchModeCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::SelectionChangedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        auto const index = SearchModeCombo().SelectedIndex();
        if (index >= 0)
        {
            ::Locket::Settings::Instance().SearchMode(static_cast<::Locket::SearchMode>(index));
        }
    }

    void GeneralSettingsPage::PasteAutomaticallyToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().PasteByDefault(PasteAutomaticallyToggle().IsOn());
        RefreshModifiers();
    }

    void GeneralSettingsPage::RemoveFormattingToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().RemoveFormattingByDefault(RemoveFormattingToggle().IsOn());
        RefreshModifiers();
    }

    void GeneralSettingsPage::ReplaceWinVToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().ReplaceWinV(ReplaceWinVToggle().IsOn());
        ApplyHotKeysAndReport();
    }

    void GeneralSettingsPage::BlockWinCToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().BlockWinC(BlockWinCToggle().IsOn());
        ApplyHotKeysAndReport();
    }

    void GeneralSettingsPage::ClipboardSettingsLink_Click(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        ::Locket::SystemIntegration::OpenWindowsClipboardSettings();
    }

    void GeneralSettingsPage::PlaySoundsToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        ::Locket::Settings::Instance().PlaySounds(PlaySoundsToggle().IsOn());
    }

    void GeneralSettingsPage::ShowNotificationsToggle_Toggled(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        bool const on = ShowNotificationsToggle().IsOn();
        ::Locket::Settings::Instance().ShowNotifications(on);
        NotificationsInfoBar().IsOpen(on && !::Locket::SystemIntegration::AreAppNotificationsEnabled());
    }

    void GeneralSettingsPage::NotificationSettingsLink_Click(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        ::Locket::SystemIntegration::OpenWindowsNotificationSettings();
    }

    void GeneralSettingsPage::OnShortcutChanged(winrt::Locket::ShortcutRecorder const& sender, winrt::Windows::Foundation::IInspectable const&)
    {
        if (m_loading)
        {
            return;
        }
        auto& settings = ::Locket::Settings::Instance();
        const auto edited = ::Locket::KeyShortcut::Parse(std::wstring_view{ sender.Shortcut() }).value_or(::Locket::KeyShortcut{});
        const auto current = sender == PopupRecorder() ? settings.PopupShortcut() :
            sender == PinRecorder() ? settings.PinShortcut() :
            sender == DeleteRecorder() ? settings.DeleteShortcut() : settings.TogglePreviewShortcut();
        const std::array<::Locket::KeyShortcut, 4> other = {
            settings.PopupShortcut(), settings.PinShortcut(), settings.DeleteShortcut(), settings.TogglePreviewShortcut() };
        bool duplicate = false;
        if (!edited.IsEmpty())
        {
            for (auto const& shortcut : other)
            {
                if (!shortcut.IsEmpty() && shortcut == edited && shortcut != current)
                {
                    duplicate = true;
                    break;
                }
            }
        }
        if (duplicate)
        {
            m_loading = true;
            sender.Shortcut(winrt::hstring{ current.ToString() });
            m_loading = false;
            HotKeyInfoBar().Title(winrt::hstring{ ::Locket::Strings::Get(L"General_HotKeyInfoBar.Title") });
            HotKeyInfoBar().Message(winrt::hstring{ ::Locket::Strings::Get(L"General_HotKeyInfoBar.Message") });
            HotKeyInfoBar().IsOpen(true);
            return;
        }
        if (sender == PopupRecorder())
        {
            settings.PopupShortcut(edited);
            ApplyHotKeysAndReport();
        }
        else if (sender == PinRecorder())
        {
            settings.PinShortcut(edited);
        }
        else if (sender == DeleteRecorder())
        {
            settings.DeleteShortcut(edited);
        }
        else if (sender == PreviewRecorder())
        {
            settings.TogglePreviewShortcut(edited);
        }
    }

    void GeneralSettingsPage::ApplyHotKeysAndReport()
    {
        bool const ok = ::Locket::AppController::Current().ApplyHotKeys();
        HotKeyInfoBar().IsOpen(!ok);
    }

    void GeneralSettingsPage::LanguageCombo_SelectionChanged(winrt::Windows::Foundation::IInspectable const&, winrt::SelectionChangedEventArgs const&)
    {
        if (m_loading)
        {
            return;
        }
        auto const index = LanguageCombo().SelectedIndex();
        auto const languages = ::Locket::Strings::Languages();
        if (index < 0 || index > static_cast<int32_t>(languages.size()))
        {
            return;
        }
        std::wstring const tag = index == 0 ? std::wstring{} : std::wstring{ languages[index - 1].tag };
        ::Locket::Settings::Instance().Language(tag);
        ::Locket::Strings::SetLanguageOverride(tag);
        // Strings are loaded when windows are created; the new language applies after a restart.
        LanguageInfoBar().IsOpen(tag != ::Locket::Strings::InitialLanguage());
    }

    void GeneralSettingsPage::LanguageRestart_Click(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            // Comes back with Settings open (App::OnActivated handles --settings).
            winrt::Microsoft::Windows::AppLifecycle::AppInstance::Restart(L"--settings");
        }
        catch (...)
        {
        }
    }
}
