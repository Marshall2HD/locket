#pragma once

#include "AboutPage.g.h"

namespace winrt::Locket::implementation
{
    struct AboutPage : AboutPageT<AboutPage>
    {
        AboutPage() = default;

        void Page_Loaded(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
        void CheckUpdatesButton_Click(winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::RoutedEventArgs const&);
    };
}

namespace winrt::Locket::factory_implementation
{
    struct AboutPage : AboutPageT<AboutPage, implementation::AboutPage>
    {
    };
}
