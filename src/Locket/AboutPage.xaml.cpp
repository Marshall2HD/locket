#include "pch.h"
#include "AboutPage.xaml.h"
#if __has_include("AboutPage.g.cpp")
#include "AboutPage.g.cpp"
#endif

#include "Platform/SystemIntegration.h"
#include "SettingsHelpers.h"
#include "Strings.h"

namespace winrt
{
    using namespace Microsoft::UI::Xaml;
}

namespace winrt::Locket::implementation
{
    void AboutPage::Page_Loaded(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        try
        {
            VersionText().Text(::Locket::Strings::Format(L"About_Version", ::Locket::SystemIntegration::AppVersion()));
            if (::Locket::SystemIntegration::InstalledFromStore())
            {
                CheckUpdatesButton().Visibility(winrt::Visibility::Collapsed);
            }
        }
        catch (...)
        {
        }
    }

    void AboutPage::CheckUpdatesButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        ::Locket::SettingsUi::CheckForUpdates(UpdateInfoBar(), CheckUpdatesButton());
    }
}
