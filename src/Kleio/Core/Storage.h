#pragma once

#include "HistoryItem.h"

#include <filesystem>
#include <vector>

struct sqlite3;

namespace Kleio
{
    // SQLite persistence (winsqlite3, ships with Windows) for history items.
    // Schema:
    //   items(id INTEGER PRIMARY KEY, application TEXT, first_copied_at INTEGER, last_copied_at INTEGER,
    //         number_of_copies INTEGER, pin TEXT, title TEXT)
    //   contents(id INTEGER PRIMARY KEY, item_id INTEGER REFERENCES items(id) ON DELETE CASCADE,
    //            type TEXT, value BLOB)
    //   contents.type is a clipboard format name (ContentType::* or any persistable format, see
    //   Core/ClipboardFormats.h); value holds its bytes.
    // Throws std::runtime_error on failures to open; other operations log and return false.
    class Storage
    {
    public:
        // Opens/creates the database. Default path: ApplicationData LocalFolder\history.sqlite.
        explicit Storage(std::filesystem::path const& path);
        static std::filesystem::path DefaultPath();
        ~Storage();
        Storage(Storage const&) = delete;
        Storage& operator=(Storage const&) = delete;

        std::vector<HistoryItemPtr> LoadAll();
        bool Insert(HistoryItem& item);          // sets item.id, writes contents; restores id on failure
        bool Update(HistoryItem const& item);    // updates item row (not contents)
        bool UpdateContents(HistoryItem const& item); // replaces contents
        bool UpdateItemAndContents(HistoryItem const& item); // atomically updates row and contents
        bool StoreReplacing(HistoryItem& replacement, std::int64_t supersededId); // atomically writes then deletes
        bool Remove(std::int64_t id);
        bool RemoveUnpinned();
        bool RemoveAll();
        std::uint64_t SizeInBytes() const;
        // Maccy's Storage.removeUnpersistableContents: deletes contents whose type is not
        // persistable (ClipboardFormats::IsPersistable). Runs when the database is opened.
        // `removed` is initialized to zero and receives the number of removed rows on success.
        bool RemoveUnpersistableContents(int& removed);

    private:
        sqlite3* m_db = nullptr;
        std::filesystem::path m_path;
    };
}
