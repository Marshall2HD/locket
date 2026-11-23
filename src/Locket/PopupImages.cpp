#include "pch.h"
#include "PopupImages.h"

#include "Platform/AppInfo.h"

#include <cmath>
#include <list>
#include <map>
#include <optional>

namespace
{
    using winrt::Microsoft::UI::Xaml::Media::Imaging::BitmapImage;

    winrt::fire_and_forget LoadPngAsync(BitmapImage image, std::shared_ptr<std::vector<std::uint8_t> const> png,
                                        std::shared_ptr<std::atomic_bool> cancelled)
    {
        auto dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        try
        {
            co_await winrt::resume_background();
            if (!png || (cancelled && cancelled->load())) co_return;
            winrt::Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            winrt::Windows::Storage::Streams::DataWriter writer{ stream };
            writer.WriteBytes(winrt::array_view<std::uint8_t const>{ png->data(), static_cast<std::uint32_t>(png->size()) });
            co_await writer.StoreAsync();
            writer.DetachStream();
            stream.Seek(0);
            if (cancelled && cancelled->load()) co_return;
            if (!dispatcher) co_return;
            co_await wil::resume_foreground(dispatcher);
            if (!cancelled || !cancelled->load())
            {
                co_await image.SetSourceAsync(stream);
            }
        }
        catch (...)
        {
            OutputDebugStringW(L"Locket: failed to decode PNG for the popup\n");
        }
    }

    struct IconKey
    {
        std::wstring path;
        int scalePercent = 100;
        bool operator<(IconKey const& other) const
        {
            return scalePercent != other.scalePercent ? scalePercent < other.scalePercent : path < other.path;
        }
    };

    template <typename Key, typename Value, std::size_t Capacity>
    class LruCache
    {
    public:
        std::optional<Value> Find(Key const& key)
        {
            if (auto found = m_index.find(key); found != m_index.end())
            {
                m_items.splice(m_items.begin(), m_items, found->second);
                return found->second->second;
            }
            return std::nullopt;
        }

        void Put(Key key, Value value)
        {
            if (auto found = m_index.find(key); found != m_index.end())
            {
                found->second->second = std::move(value);
                m_items.splice(m_items.begin(), m_items, found->second);
                return;
            }
            m_items.emplace_front(std::move(key), std::move(value));
            m_index[m_items.front().first] = m_items.begin();
            if (m_items.size() > Capacity)
            {
                m_index.erase(m_items.back().first);
                m_items.pop_back();
            }
        }

    private:
        std::list<std::pair<Key, Value>> m_items;
        std::map<Key, typename std::list<std::pair<Key, Value>>::iterator> m_index;
    };

    // Intentionally leaked: projected WinRT objects must not be released during static destruction.
    LruCache<IconKey, BitmapImage, 256>& IconCache()
    {
        static auto* cache = new LruCache<IconKey, BitmapImage, 256>();
        return *cache;
    }

    LruCache<std::wstring, std::wstring, 512>& NameCache()
    {
        static auto* cache = new LruCache<std::wstring, std::wstring, 512>();
        return *cache;
    }
}

namespace Locket::PopupImages
{
    PixelSize PngSize(std::vector<std::uint8_t> const& png) noexcept
    {
        static constexpr std::uint8_t signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (png.size() < 24 || !std::equal(std::begin(signature), std::end(signature), png.begin()) ||
            png[12] != 'I' || png[13] != 'H' || png[14] != 'D' || png[15] != 'R') return {};
        auto readBigEndian = [&](std::size_t offset) {
            return (static_cast<std::uint32_t>(png[offset]) << 24) | (static_cast<std::uint32_t>(png[offset + 1]) << 16) |
                   (static_cast<std::uint32_t>(png[offset + 2]) << 8) | static_cast<std::uint32_t>(png[offset + 3]);
        };
        return { readBigEndian(16), readBigEndian(20) };
    }

    BitmapImage FromPng(std::shared_ptr<std::vector<std::uint8_t> const> png, std::shared_ptr<std::atomic_bool> cancelled,
                        int decodePixelHeight, int decodePixelWidth)
    {
        BitmapImage image;
        if (decodePixelHeight > 0) image.DecodePixelHeight(decodePixelHeight);
        else if (decodePixelWidth > 0) image.DecodePixelWidth(decodePixelWidth);
        if (png && !png->empty()) LoadPngAsync(image, std::move(png), std::move(cancelled));
        return image;
    }

    BitmapImage AppIcon(std::wstring const& exePath, double scale)
    {
        if (exePath.empty()) return nullptr;
        IconKey key{ exePath, static_cast<int>(std::lround(scale * 100.0)) };
        if (auto image = IconCache().Find(key)) return *image;
        BitmapImage image{ nullptr };
        try
        {
            auto bytes = Locket::AppInfo::IconPng(exePath, scale);
            if (!bytes.empty())
            {
                image = FromPng(std::make_shared<std::vector<std::uint8_t> const>(std::move(bytes)), {},
                                static_cast<int>(std::lround(16.0 * scale)));
            }
        }
        catch (...) {}
        IconCache().Put(std::move(key), image);
        return image;
    }

    std::wstring AppName(std::wstring const& exePath)
    {
        if (auto name = NameCache().Find(exePath)) return *name;
        std::wstring name;
        try { name = Locket::AppInfo::DisplayName(exePath); } catch (...) {}
        if (name.empty()) name = Locket::AppInfo::FileName(exePath);
        NameCache().Put(exePath, name);
        return name;
    }
}
