#include "pch.h"
#include "Core/History.h"
#include "Core/Sorter.h"

#include <algorithm>
#include <limits>
#include <random>

namespace Kleio
{
    namespace
    {
        // Maccy's HistoryItem.supportedPins with the Windows reservations documented in History.h:
        // a (select all), c (copy), q (quit), v (paste), w (close), x (cut), y (redo), z (undo), and
        // the letters of Maccy's Emacs-style popup keys (KeyChord): h (delete a character), j / n
        // (next), k / p (previous), u (clear the search), w (delete a word). Maccy can keep those
        // letters because its pins use Cmd while the Emacs keys use Control; Kleio maps both to Ctrl.
        constexpr std::wstring_view kBasePins[] = {
            L"b", L"d", L"e", L"f", L"g", L"i", L"l", L"m", L"o", L"r", L"s", L"t",
        };

        bool Erase(std::vector<HistoryItemPtr>& items, HistoryItemPtr const& item)
        {
            const auto it = std::find(items.begin(), items.end(), item);
            if (it == items.end())
            {
                return false;
            }
            items.erase(it);
            return true;
        }

        bool Contains(std::vector<std::wstring> const& values, std::wstring const& value)
        {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        std::vector<std::wstring> UniquePins(std::vector<std::wstring> const& pins)
        {
            std::vector<std::wstring> unique;
            for (auto const& pin : pins)
            {
                if (!pin.empty() && !Contains(unique, pin))
                {
                    unique.push_back(pin);
                }
            }
            return unique;
        }

        std::mt19937& RandomEngine()
        {
            static std::mt19937 engine{ std::random_device{}() };
            return engine;
        }

        std::int32_t SaturatingCopies(std::int32_t left, std::int32_t right) noexcept
        {
            const auto sum = static_cast<std::int64_t>(left) + static_cast<std::int64_t>(right);
            return sum > std::numeric_limits<std::int32_t>::max()
                ? std::numeric_limits<std::int32_t>::max()
                : static_cast<std::int32_t>(std::max<std::int64_t>(1, sum));
        }
    }

    History::History(Storage& storage, Settings& settings) : m_storage(storage), m_settings(settings)
    {
        m_settingsToken = m_settings.Changed.Add([this](std::wstring_view key) { OnSettingChanged(key); });
    }

    History::~History()
    {
        m_settings.Changed.Remove(m_settingsToken);
    }

    void History::OnSettingChanged(std::wstring_view key)
    {
        if (key == Settings::kSortBy || key == Settings::kPinTo)
        {
            Resort();
            Changed();
        }
        else if (key == Settings::kPinShortcut || key == Settings::kDeleteShortcut || key == Settings::kTogglePreviewShortcut)
        {
            ReassignReservedPins();
            ReconcilePinOrder();
            SortPinned();
            Changed();
        }
        else if (key == Settings::kShowSpecialSymbols)
        {
            RegenerateTitles();
        }
        else if (key == Settings::kHistorySize)
        {
            Trim();
            Changed();
        }
    }

    void History::Load()
    {
        auto sorted = SortItems(m_storage.LoadAll(), m_settings.SortBy(), PinsPosition::Top, m_settings.PinOrder());
        m_pinned.clear();
        m_unpinned.clear();
        for (auto& item : sorted)
        {
            if (item->IsPinned())
            {
                m_pinned.push_back(std::move(item));
            }
            else
            {
                m_unpinned.push_back(std::move(item));
            }
        }
        ReassignReservedPins();
        ReconcilePinOrder();
        SortPinned();
        Trim();
        Changed();
    }

    void History::ReassignReservedPins()
    {
        // Pins stored by an older version may use a letter that is reserved now (e.g. "n", taken by
        // Ctrl+N). Give such items a random free letter, keeping their place in the pin order; when
        // no letter is free the old one is kept (the reserved key wins, the item stays pinned).
        for (auto const& item : m_pinned)
        {
            if (item->pin.empty() || Contains(SupportedPins(), item->pin))
            {
                continue;
            }
            const auto pins = AvailablePins();
            if (pins.empty())
            {
                return;
            }
            std::uniform_int_distribution<std::size_t> distribution(0, pins.size() - 1);
            const std::wstring oldPin = item->pin;
            const std::wstring newPin = pins[distribution(RandomEngine())];
            item->pin = newPin;
            if (!m_storage.Update(*item))
            {
                item->pin = oldPin;
                continue;
            }

            // PinOrder.replace(oldPin, with: pin)
            auto order = UniquePins(m_settings.PinOrder());
            std::erase(order, newPin);
            if (auto it = std::find(order.begin(), order.end(), oldPin); it != order.end())
            {
                *it = newPin;
            }
            else
            {
                order.push_back(newPin);
            }
            m_settings.PinOrder(order);
        }
    }

    HistoryItemPtr History::Add(HistoryItemPtr item)
    {
        if (!item)
        {
            return item;
        }

        // Find the successor before changing either list. Its write and deletion of the old row are
        // one transaction, so an unavailable database cannot destroy an existing duplicate.
        HistoryItemPtr existing;
        for (auto const* list : { &m_pinned, &m_unpinned })
        {
            for (auto const& candidate : *list)
            {
                if (candidate != item && candidate->Supersedes(*item))
                {
                    existing = candidate;
                    break;
                }
            }
            if (existing)
            {
                break;
            }
        }
        HistoryItemPtr modified = IsModified(*item);
        if (modified == item)
        {
            modified = nullptr;
        }
        if (!existing)
        {
            existing = modified;
        }

        const HistoryItem original = *item;
        const bool wasDuplicate = existing != nullptr;
        if (existing)
        {
            if (!modified)
            {
                item->contents = existing->contents;
            }
            item->firstCopiedAt = existing->firstCopiedAt;
            item->numberOfCopies = SaturatingCopies(item->numberOfCopies, existing->numberOfCopies);
            item->pin = existing->pin;
            item->title = existing->title;
            if (!item->FromKleio())
            {
                item->application = existing->application;
            }
        }
        else if (item->title.empty() && !item->IsImage())
        {
            item->title = item->GenerateTitle(m_settings.ShowSpecialSymbols());
        }

        if (!m_storage.StoreReplacing(*item, existing ? existing->id : 0))
        {
            *item = original;
            return nullptr;
        }

        // Persistence succeeded: now update the in-memory model and settings, then trim durable
        // overflow. Removing an item from either list first also handles re-adding the same object.
        Erase(m_pinned, item);
        Erase(m_unpinned, item);
        if (existing)
        {
            Erase(m_pinned, existing);
            Erase(m_unpinned, existing);
            for (auto& entry : m_sessionLog)
            {
                if (entry.second == existing)
                {
                    entry.second = item;
                }
            }
        }
        if (item->IsPinned())
        {
            m_pinned.push_back(item);
            ReconcilePinOrder();
            SortPinned();
        }
        else
        {
            InsertUnpinned(item);
        }
        if (item->changeCount != 0)
        {
            m_sessionLog[item->changeCount] = item;
        }
        TrimTo(static_cast<std::size_t>(m_settings.HistorySize()));
        ItemAdded(item, wasDuplicate);
        Changed();
        return item;
    }

    void History::Delete(HistoryItemPtr const& item)
    {
        if (!item)
        {
            return;
        }
        if (!m_storage.Remove(item->id))
        {
            return;
        }
        Erase(m_pinned, item);
        Erase(m_unpinned, item);
        ForgetInSessionLog(item);
        if (item->IsPinned())
        {
            RemovePinFromOrderIfUnused(item->pin);
        }
        Changed();
    }

    void History::Clear()
    {
        if (!m_storage.RemoveUnpinned())
        {
            return;
        }
        m_unpinned.clear();
        std::erase_if(m_sessionLog, [](auto const& entry) { return !entry.second->IsPinned(); });
        Changed();
    }

    void History::ClearAll()
    {
        if (!m_storage.RemoveAll())
        {
            return;
        }
        m_pinned.clear();
        m_unpinned.clear();
        m_sessionLog.clear();
        m_settings.PinOrder({});
        Changed();
    }

    void History::TogglePin(HistoryItemPtr const& item)
    {
        if (!item)
        {
            return;
        }

        if (item->IsPinned())
        {
            const std::wstring pin = item->pin;
            item->pin.clear();
            if (!m_storage.Update(*item))
            {
                item->pin = pin;
                return;
            }
            Erase(m_pinned, item);
            RemovePinFromOrderIfUnused(pin);
            Erase(m_unpinned, item);
            InsertUnpinned(item);
        }
        else
        {
            const auto pins = AvailablePins();
            if (pins.empty())
            {
                return;   // Kleio cannot represent Maccy's "pinned without shortcut" state
            }
            std::uniform_int_distribution<std::size_t> distribution(0, pins.size() - 1);
            const std::wstring pin = pins[distribution(RandomEngine())];
            item->pin = pin;
            if (!m_storage.Update(*item))
            {
                item->pin.clear();
                return;
            }
            Erase(m_unpinned, item);
            if (std::find(m_pinned.begin(), m_pinned.end(), item) == m_pinned.end())
            {
                m_pinned.push_back(item);
            }
            auto order = UniquePins(m_settings.PinOrder());
            if (!Contains(order, item->pin))
            {
                order.push_back(item->pin);
            }
            m_settings.PinOrder(order);
            SortPinned();
        }
        Changed();
    }

    void History::UpdatePin(HistoryItemPtr const& item, std::wstring const& pin)
    {
        if (!item)
        {
            return;
        }
        const auto supported = SupportedPins();
        if (!Contains(supported, pin) || item->pin == pin)
        {
            return;
        }
        const bool taken = std::any_of(m_pinned.begin(), m_pinned.end(), [&](HistoryItemPtr const& other) {
            return other != item && other->pin == pin;
        });
        if (taken)
        {
            return;
        }

        const std::wstring oldPin = item->pin;
        item->pin = pin;
        if (!m_storage.Update(*item))
        {
            item->pin = oldPin;
            return;
        }
        if (std::find(m_pinned.begin(), m_pinned.end(), item) == m_pinned.end())
        {
            m_pinned.push_back(item);
        }
        Erase(m_unpinned, item);

        // PinOrder.replace(oldPin, with: pin)
        auto order = UniquePins(m_settings.PinOrder());
        std::erase(order, pin);
        const auto oldIt = oldPin.empty() ? order.end() : std::find(order.begin(), order.end(), oldPin);
        if (oldIt != order.end())
        {
            *oldIt = pin;
        }
        else
        {
            order.push_back(pin);
        }
        m_settings.PinOrder(order);

        SortPinned();
        Changed();
    }

    void History::MovePin(std::size_t from, std::size_t to)
    {
        if (from >= m_pinned.size())
        {
            return;
        }
        to = std::min(to, m_pinned.size() - 1);
        if (from != to)
        {
            auto item = m_pinned[from];
            m_pinned.erase(m_pinned.begin() + static_cast<std::ptrdiff_t>(from));
            m_pinned.insert(m_pinned.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
        }
        std::vector<std::wstring> order;
        for (auto const& pinned : m_pinned)
        {
            order.push_back(pinned->pin);
        }
        m_settings.PinOrder(UniquePins(order));
        Changed();
    }

    void History::UpdateTitle(HistoryItemPtr const& item, std::wstring title)
    {
        if (!item)
        {
            return;
        }
        const std::wstring previousTitle = item->title;
        item->title = std::move(title);
        if (!m_storage.Update(*item))
        {
            item->title = previousTitle;
            return;
        }
        Changed();
    }

    void History::UpdateText(HistoryItemPtr const& item, std::wstring const& text)
    {
        if (!item)
        {
            return;
        }
        const auto previousContents = item->contents;
        const std::wstring previousTitle = item->title;
        // Like Maccy's ItemEditorView.applyChanges: only the edited plain text is kept, so none of
        // the other stored formats can paste the old content.
        std::erase_if(item->contents, [](HistoryItemContent const& content) { return content.type != ContentType::Text; });
        if (item->contents.empty())
        {
            item->contents.push_back(HistoryItemContent{ std::wstring{ ContentType::Text }, {} });
        }
        item->contents.front().value = EncodeText(text);
        item->title = item->GenerateTitle(m_settings.ShowSpecialSymbols());
        if (!m_storage.UpdateItemAndContents(*item))
        {
            item->contents = previousContents;
            item->title = previousTitle;
            return;
        }
        Changed();
    }

    void History::RegenerateTitles()
    {
        const bool showSpecialSymbols = m_settings.ShowSpecialSymbols();
        bool changed = false;
        for (auto const* list : { &m_pinned, &m_unpinned })
        {
            for (auto const& item : *list)
            {
                if (item->IsImage())
                {
                    continue;   // OCR titles are kept
                }
                auto title = item->GenerateTitle(showSpecialSymbols);
                if (title == item->title)
                {
                    continue;
                }
                const std::wstring previousTitle = item->title;
                item->title = std::move(title);
                if (m_storage.Update(*item))
                {
                    changed = true;
                    continue;
                }
                item->title = previousTitle;
            }
        }
        if (changed)
        {
            Changed();
        }
    }

    std::vector<HistoryItemPtr> History::AllItems() const
    {
        std::vector<HistoryItemPtr> items;
        items.reserve(m_pinned.size() + m_unpinned.size());
        if (m_settings.PinTo() == PinsPosition::Bottom)
        {
            items.insert(items.end(), m_unpinned.begin(), m_unpinned.end());
            items.insert(items.end(), m_pinned.begin(), m_pinned.end());
        }
        else
        {
            items.insert(items.end(), m_pinned.begin(), m_pinned.end());
            items.insert(items.end(), m_unpinned.begin(), m_unpinned.end());
        }
        return items;
    }

    HistoryItemPtr History::FindById(std::int64_t id) const
    {
        for (auto const* list : { &m_pinned, &m_unpinned })
        {
            for (auto const& item : *list)
            {
                if (item->id == id)
                {
                    return item;
                }
            }
        }
        return nullptr;
    }

    HistoryItemPtr History::MostRecent() const
    {
        HistoryItemPtr result;
        for (auto const* list : { &m_unpinned, &m_pinned })
        {
            for (auto const& item : *list)
            {
                if (!result || item->lastCopiedAt > result->lastCopiedAt)
                {
                    result = item;
                }
            }
        }
        return result;
    }

    std::vector<std::wstring> History::SupportedPins() const
    {
        std::vector<std::wstring> pins;
        for (auto pin : kBasePins)
        {
            pins.emplace_back(pin);
        }
        for (auto const& shortcut : { m_settings.PinShortcut(), m_settings.DeleteShortcut(), m_settings.TogglePreviewShortcut() })
        {
            if (shortcut.key >= 'A' && shortcut.key <= 'Z')
            {
                const std::wstring letter(1, static_cast<wchar_t>(L'a' + (shortcut.key - 'A')));
                std::erase(pins, letter);
            }
        }
        return pins;
    }

    std::vector<std::wstring> History::AvailablePins() const
    {
        auto pins = SupportedPins();
        std::erase_if(pins, [this](std::wstring const& pin) {
            return std::any_of(m_pinned.begin(), m_pinned.end(), [&pin](HistoryItemPtr const& item) { return item->pin == pin; });
        });
        std::sort(pins.begin(), pins.end());
        return pins;
    }

    History::Filtered History::Filter(std::wstring_view query) const
    {
        const auto mode = m_settings.SearchMode();
        Filtered filtered;
        filtered.pinned = m_search.Run(query, m_pinned, mode);
        filtered.unpinned = m_search.Run(query, m_unpinned, mode);
        return filtered;
    }

    // ------------------------------------------------------------------ private

    void History::Resort()
    {
        const auto by = m_settings.SortBy();
        std::stable_sort(m_unpinned.begin(), m_unpinned.end(), [by](HistoryItemPtr const& a, HistoryItemPtr const& b) {
            return SortsBefore(*a, *b, by);
        });
        SortPinned();
    }

    void History::Trim()
    {
        TrimTo(static_cast<std::size_t>(m_settings.HistorySize()));
    }

    void History::TrimTo(std::size_t maxUnpinned)
    {
        while (m_unpinned.size() > maxUnpinned)
        {
            auto item = m_unpinned.back();
            if (!m_storage.Remove(item->id))
            {
                return;
            }
            m_unpinned.pop_back();
            ForgetInSessionLog(item);
        }
    }

    void History::InsertUnpinned(HistoryItemPtr const& item)
    {
        const auto by = m_settings.SortBy();
        // Before the first item that does not sort before the new one, so a fresh copy wins ties.
        const auto position = std::find_if(m_unpinned.begin(), m_unpinned.end(), [&](HistoryItemPtr const& existing) {
            return !SortsBefore(*existing, *item, by);
        });
        m_unpinned.insert(position, item);
    }

    void History::SortPinned()
    {
        const auto order = m_settings.PinOrder();
        auto rank = [&order](HistoryItemPtr const& item) -> std::size_t {
            const auto it = std::find(order.begin(), order.end(), item->pin);
            return it == order.end() ? order.size() : static_cast<std::size_t>(it - order.begin());
        };
        std::stable_sort(m_pinned.begin(), m_pinned.end(), [&rank](HistoryItemPtr const& a, HistoryItemPtr const& b) {
            return rank(a) < rank(b);
        });
    }

    void History::ReconcilePinOrder()
    {
        std::vector<std::wstring> assigned;
        for (auto const& item : m_pinned)
        {
            if (!item->pin.empty() && !Contains(assigned, item->pin))
            {
                assigned.push_back(item->pin);
            }
        }
        auto order = UniquePins(m_settings.PinOrder());
        std::erase_if(order, [&assigned](std::wstring const& pin) { return !Contains(assigned, pin); });
        for (auto const& pin : assigned)
        {
            if (!Contains(order, pin))
            {
                order.push_back(pin);
            }
        }
        m_settings.PinOrder(order);
    }

    HistoryItemPtr History::IsModified(HistoryItem const& item) const
    {
        if (const auto modified = item.Modified())
        {
            if (const auto it = m_sessionLog.find(*modified); it != m_sessionLog.end())
            {
                return it->second;
            }
        }
        return nullptr;
    }

    void History::ForgetInSessionLog(HistoryItemPtr const& item)
    {
        std::erase_if(m_sessionLog, [&item](auto const& entry) { return entry.second == item; });
    }

    void History::RemovePinFromOrderIfUnused(std::wstring const& pin)
    {
        const bool stillUsed = std::any_of(m_pinned.begin(), m_pinned.end(), [&pin](HistoryItemPtr const& item) { return item->pin == pin; });
        if (stillUsed)
        {
            return;
        }
        auto order = m_settings.PinOrder();
        if (std::erase(order, pin) > 0)
        {
            m_settings.PinOrder(order);
        }
    }
}
