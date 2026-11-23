#include "pch.h"
#include "Platform/ScriptingPipe.h"

#include "Strings.h"

#include <appmodel.h>
#include <sddl.h>
#include <shobjidl.h>

#include <filesystem>
#include <future>

#pragma comment(lib, "Advapi32.lib")

namespace Locket::ScriptingPipe
{
    using namespace Locket::Scripting;
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValue;

    namespace
    {
        constexpr std::uint32_t MaxRequestSize = 64 * 1024;
        constexpr std::uint32_t MaxPayloadSize = 64u * 1024u * 1024u;
        constexpr DWORD TransferTimeoutMs = 30'000;
        // How long the client waits for Locket (including starting it) before giving up.
        constexpr ULONGLONG ConnectTimeoutMs = 20'000;

        void Log(std::wstring_view message)
        {
            std::wstring line{ L"[Locket] scripting: " };
            line.append(message);
            line.push_back(L'\n');
            ::OutputDebugStringW(line.c_str());
        }

        // ---- Framing ----

        // Overlapped client transfer with a finite deadline. Cancel and drain every timed-out I/O.
        bool TransferSync(HANDLE pipe, bool write, std::uint8_t* data, std::size_t size)
        {
            wil::unique_event io{ wil::EventOptions::ManualReset };
            while (size > 0)
            {
                const DWORD chunk = static_cast<DWORD>((std::min)(size, std::size_t{ 1 } << 20));
                OVERLAPPED overlapped{};
                overlapped.hEvent = io.get();
                io.ResetEvent();
                const BOOL started = write ? ::WriteFile(pipe, data, chunk, nullptr, &overlapped) : ::ReadFile(pipe, data, chunk, nullptr, &overlapped);
                if (!started && ::GetLastError() != ERROR_IO_PENDING) return false;
                if (::WaitForSingleObject(io.get(), TransferTimeoutMs) != WAIT_OBJECT_0)
                {
                    ::CancelIoEx(pipe, &overlapped);
                    DWORD ignored = 0;
                    ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                    return false;
                }
                DWORD done = 0;
                if (!::GetOverlappedResult(pipe, &overlapped, &done, FALSE) || done == 0) return false;
                data += done;
                size -= done;
            }
            return true;
        }

        // Overlapped (server side) transfer that gives up when `stop` is signalled.
        bool TransferAsync(HANDLE pipe, HANDLE stop, bool write, std::uint8_t* data, std::size_t size)
        {
            wil::unique_event io{ wil::EventOptions::ManualReset };
            HANDLE handles[2] = { io.get(), stop };
            while (size > 0)
            {
                DWORD const chunk = static_cast<DWORD>((std::min)(size, std::size_t{ 1 } << 20));
                OVERLAPPED overlapped{};
                overlapped.hEvent = io.get();
                io.ResetEvent();
                BOOL const ok = write ? ::WriteFile(pipe, data, chunk, nullptr, &overlapped) : ::ReadFile(pipe, data, chunk, nullptr, &overlapped);
                if (!ok && ::GetLastError() != ERROR_IO_PENDING)
                {
                    return false;
                }
                const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, TransferTimeoutMs);
                if (wait != WAIT_OBJECT_0)
                {
                    ::CancelIoEx(pipe, &overlapped);
                    DWORD ignored = 0;
                    ::GetOverlappedResult(pipe, &overlapped, &ignored, TRUE);
                    return false;
                }
                DWORD done = 0;
                if (!::GetOverlappedResult(pipe, &overlapped, &done, FALSE) || done == 0)
                {
                    return false;
                }
                data += done;
                size -= done;
            }
            return true;
        }

        template <typename Transfer>
        bool WriteFrame(Transfer&& transfer, std::span<std::uint8_t const> bytes)
        {
            std::uint32_t length = static_cast<std::uint32_t>(bytes.size());
            std::uint8_t header[4] = {
                static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8),
                static_cast<std::uint8_t>(length >> 16), static_cast<std::uint8_t>(length >> 24) };
            return transfer(true, header, sizeof(header)) &&
                (bytes.empty() || transfer(true, const_cast<std::uint8_t*>(bytes.data()), bytes.size()));
        }

        template <typename Transfer>
        std::optional<std::vector<std::uint8_t>> ReadFrame(Transfer&& transfer, std::uint32_t maximum)
        {
            std::uint8_t header[4]{};
            if (!transfer(false, header, sizeof(header)))
            {
                return std::nullopt;
            }
            std::uint32_t const length = header[0] | (header[1] << 8) | (header[2] << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
            if (length > maximum)
            {
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(length);
            if (length > 0 && !transfer(false, bytes.data(), bytes.size()))
            {
                return std::nullopt;
            }
            return bytes;
        }

        std::span<std::uint8_t const> AsBytes(std::string const& text)
        {
            return { reinterpret_cast<std::uint8_t const*>(text.data()), text.size() };
        }

        std::wstring AsWide(std::vector<std::uint8_t> const& bytes)
        {
            return FromUtf8(std::string_view{ reinterpret_cast<char const*>(bytes.data()), bytes.size() });
        }

        // ---- Serialization ----

        std::string SerializeCommand(Command const& command)
        {
            JsonObject json;
            json.SetNamedValue(L"verb", JsonValue::CreateStringValue(VerbName(command.verb)));
            if (command.number)
            {
                json.SetNamedValue(L"number", JsonValue::CreateNumberValue(*command.number));
            }
            json.SetNamedValue(L"format", JsonValue::CreateStringValue(FormatName(command.format)));
            json.SetNamedValue(L"yes", JsonValue::CreateBooleanValue(command.yes));
            return ToUtf8(json.Stringify());
        }

        std::optional<Command> DeserializeCommand(std::vector<std::uint8_t> const& bytes)
        {
            JsonObject json{ nullptr };
            if (!JsonObject::TryParse(AsWide(bytes), json) || !json)
            {
                return std::nullopt;
            }
            Command command;
            auto verb = VerbFromName(json.GetNamedString(L"verb", L""));
            auto format = FormatFromName(json.GetNamedString(L"format", L"auto"));
            if (!verb || !format)
            {
                return std::nullopt;
            }
            command.verb = *verb;
            command.format = *format;
            if (json.HasKey(L"number"))
            {
                double const number = json.GetNamedNumber(L"number", 0);
                if (number < 1 || number > 1'000'000'000)
                {
                    command.number = 0;     // reported as not found
                }
                else
                {
                    command.number = static_cast<std::int32_t>(number);
                }
            }
            command.yes = json.GetNamedBoolean(L"yes", false);
            return command;
        }

        std::string SerializeResultHeader(Result const& result)
        {
            JsonObject json;
            json.SetNamedValue(L"exitCode", JsonValue::CreateNumberValue(result.exitCode));
            json.SetNamedValue(L"error", JsonValue::CreateStringValue(result.error));
            json.SetNamedValue(L"format", JsonValue::CreateStringValue(FormatName(result.payload.format)));
            return ToUtf8(json.Stringify());
        }

        // ---- Console output (client) ----

        // The alias process is a GUI-subsystem executable: it has real standard handles only when
        // the caller redirected them (pipes, files); otherwise it attaches to the caller's console.
        struct OutputStream
        {
            HANDLE handle = nullptr;
            bool console = false;
        };

        HANDLE AttachedConsoleOutput()
        {
            static HANDLE output = [] {
                if (!::AttachConsole(ATTACH_PARENT_PROCESS))
                {
                    return INVALID_HANDLE_VALUE;
                }
                return ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            }();
            return output;
        }

        OutputStream Stream(DWORD which)
        {
            HANDLE handle = ::GetStdHandle(which);
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE && ::GetFileType(handle) != FILE_TYPE_UNKNOWN)
            {
                return { handle, ::GetFileType(handle) == FILE_TYPE_CHAR };
            }
            HANDLE console = AttachedConsoleOutput();
            if (console != INVALID_HANDLE_VALUE)
            {
                return { console, true };
            }
            return {};
        }

        // Console: UTF-16 through WriteConsoleW (independent of the console code page), with a final
        // line break. Redirected: the exact bytes (UTF-8 for text).
        void Write(DWORD which, std::span<std::uint8_t const> bytes, bool text)
        {
            auto const stream = Stream(which);
            if (stream.handle == nullptr)
            {
                return;
            }
            if (stream.console && text)
            {
                std::wstring wide = FromUtf8(std::string_view{ reinterpret_cast<char const*>(bytes.data()), bytes.size() });
                if (wide.empty() || wide.back() != L'\n')
                {
                    wide += L"\r\n";
                }
                DWORD written = 0;
                if (::WriteConsoleW(stream.handle, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr))
                {
                    return;
                }
                // Not a console after all (e.g. NUL): write the bytes.
            }
            std::size_t offset = 0;
            while (offset < bytes.size())
            {
                DWORD written = 0;
                DWORD const chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{ 1 } << 20));
                if (!::WriteFile(stream.handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0)
                {
                    return;
                }
                offset += written;
            }
        }

        void WriteText(DWORD which, std::wstring_view text)
        {
            const std::string utf8 = ToUtf8(text);
            Write(which, AsBytes(utf8), true);
        }

        int Fail(int exitCode, std::wstring const& message)
        {
            WriteText(STD_ERROR_HANDLE, L"locket: " + message + L"\r\n");
            return exitCode;
        }

        std::string Base64(std::span<std::uint8_t const> bytes)
        {
            static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string result;
            result.reserve((bytes.size() + 2) / 3 * 4);
            std::size_t i = 0;
            for (; i + 2 < bytes.size(); i += 3)
            {
                std::uint32_t const n = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
                result += Alphabet[(n >> 18) & 63];
                result += Alphabet[(n >> 12) & 63];
                result += Alphabet[(n >> 6) & 63];
                result += Alphabet[n & 63];
            }
            if (i < bytes.size())
            {
                std::uint32_t n = bytes[i] << 16;
                if (i + 1 < bytes.size())
                {
                    n |= bytes[i + 1] << 8;
                }
                result += Alphabet[(n >> 18) & 63];
                result += Alphabet[(n >> 12) & 63];
                result += i + 1 < bytes.size() ? Alphabet[(n >> 6) & 63] : '=';
                result += '=';
            }
            return result;
        }

        // Starts the main instance in the background (no popup), like an App Intent launching Maccy.
        bool LaunchLocket()
        {
            UINT32 length = 0;
            std::wstring aumid;
            if (::GetCurrentApplicationUserModelId(&length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0)
            {
                aumid.resize(length);
                if (::GetCurrentApplicationUserModelId(&length, aumid.data()) == ERROR_SUCCESS)
                {
                    aumid.resize(length > 0 ? length - 1 : 0);
                }
                else
                {
                    aumid.clear();
                }
            }
            if (!aumid.empty())
            {
                try
                {
                    auto manager = winrt::create_instance<IApplicationActivationManager>(CLSID_ApplicationActivationManager, CLSCTX_LOCAL_SERVER);
                    DWORD processId = 0;
                    if (SUCCEEDED(manager->ActivateApplication(aumid.c_str(), L"--background", AO_NOERRORUI, &processId)))
                    {
                        return true;
                    }
                }
                catch (...)
                {
                }
            }

#ifndef LOCKET_STORE_BUILD
            // Not packaged (development): start our own executable.
            std::wstring path(MAX_PATH, L'\0');
            DWORD const size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (size == 0 || size >= path.size())
            {
                return false;
            }
            path.resize(size);
            std::wstring commandLine = L"\"" + path + L"\" --background";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            if (!::CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
            {
                return false;
            }
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            return true;
#else
            return false;
#endif
        }

        bool IsLocketRunning()
        {
            try
            {
                for (auto const& instance : winrt::Microsoft::Windows::AppLifecycle::AppInstance::GetInstances())
                {
                    if (instance.Key() == L"Locket.Main")
                    {
                        return true;
                    }
                }
            }
            catch (...)
            {
            }
            return false;
        }

        bool IsExpectedServer(HANDLE pipe)
        {
            ULONG processId = 0;
            if (!::GetNamedPipeServerProcessId(pipe, &processId) || processId == 0) return false;
            wil::unique_handle process{ ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) };
            if (!process) return false;

            UINT32 familyLength = 0;
            const LONG familyStatus = ::GetCurrentPackageFamilyName(&familyLength, nullptr);
            if (familyStatus == ERROR_INSUFFICIENT_BUFFER && familyLength > 0)
            {
                std::wstring expected(familyLength, L'\0');
                std::wstring actual(familyLength, L'\0');
                if (::GetCurrentPackageFamilyName(&familyLength, expected.data()) == ERROR_SUCCESS &&
                    ::GetPackageFamilyName(process.get(), &familyLength, actual.data()) == ERROR_SUCCESS)
                {
                    expected.resize(expected.find(L'\0'));
                    actual.resize(actual.find(L'\0'));
                    return CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) == CSTR_EQUAL;
                }
                return false;
            }

            std::wstring expected(MAX_PATH, L'\0');
            DWORD expectedLength = static_cast<DWORD>(expected.size());
            std::wstring actual(MAX_PATH, L'\0');
            DWORD actualLength = static_cast<DWORD>(actual.size());
            if (!::QueryFullProcessImageNameW(::GetCurrentProcess(), 0, expected.data(), &expectedLength) ||
                !::QueryFullProcessImageNameW(process.get(), 0, actual.data(), &actualLength)) return false;
            expected.resize(expectedLength);
            actual.resize(actualLength);
            return CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) == CSTR_EQUAL;
        }

        wil::unique_hfile Connect(int& exitCode, std::wstring& error)
        {
            auto const name = Name();
            auto const deadline = ::GetTickCount64() + ConnectTimeoutMs;
            bool launched = false;
            for (;;)
            {
                wil::unique_hfile pipe{ ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                                      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | FILE_FLAG_OVERLAPPED, nullptr) };
                if (pipe)
                {
                    return pipe;
                }
                DWORD const lastError = ::GetLastError();
                if (::GetTickCount64() > deadline)
                {
                    exitCode = Unavailable;
                    error = Strings::Get(launched ? L"Script_NoResponseAfterStart" : L"Script_NotResponding");
                    return {};
                }
                if (lastError == ERROR_PIPE_BUSY)
                {
                    ::WaitNamedPipeW(name.c_str(), 2000);
                    continue;
                }
                // The pipe is briefly missing between two requests; only start Locket when no
                // main instance is registered.
                if (lastError == ERROR_FILE_NOT_FOUND && !launched && !IsLocketRunning())
                {
                    if (!LaunchLocket())
                    {
                        exitCode = Unavailable;
                        error = Strings::Get(L"Script_CouldNotStart");
                        return {};
                    }
                    launched = true;
                }
                ::Sleep(150);
            }
        }
    }

    std::wstring Name()
    {
        std::wstring sid = L"user";
        wil::unique_handle token;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put()))
        {
            DWORD size = 0;
            ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
            std::vector<std::uint8_t> buffer(size);
            if (size > 0 && ::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size))
            {
                LPWSTR text = nullptr;
                if (::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text))
                {
                    sid = text;
                    ::LocalFree(text);
                }
            }
        }
        DWORD session = 0;
        ::ProcessIdToSessionId(::GetCurrentProcessId(), &session);
        return L"\\\\.\\pipe\\Locket.Scripting." + sid + L"." + std::to_wstring(session);
    }

        bool PipeSecurityAttributes(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor)
        {
            wil::unique_handle token;
            if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, token.put())) return false;
            DWORD size = 0;
            ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
            std::vector<std::uint8_t> buffer(size);
            if (size == 0 || !::GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) return false;
            LPWSTR sidText = nullptr;
            if (!::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sidText)) return false;
            const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring{ sidText } + L")(A;;GA;;;SY)";
            ::LocalFree(sidText);
            if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
            attributes = { sizeof(attributes), descriptor, FALSE };
            return true;
        }

        // ---- Server ----

    Server::Server(Handler handler) :
        m_handler(std::move(handler)),
        m_dispatcher(winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread()),
        m_stop(wil::EventOptions::ManualReset)
    {
        m_thread = std::thread([this] { Run(); });
    }

    Server::~Server()
    {
        m_stopping = true;
        m_stop.SetEvent();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        std::scoped_lock lock{ m_workersMutex };
        for (auto& worker : m_workers)
        {
            if (worker.joinable()) worker.join();
        }
        m_workers.clear();
    }

    void Server::Run()
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto const name = Name();
        wil::unique_event connected{ wil::EventOptions::ManualReset };
        while (!m_stopping)
        {
            SECURITY_ATTRIBUTES attributes{};
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!PipeSecurityAttributes(attributes, descriptor))
            {
                Log(L"could not create pipe security attributes");
                break;
            }
            // descriptor remains allocated through CreateNamedPipeW and is released immediately after.
            HANDLE const rawPipe = ::CreateNamedPipeW(
                name.c_str(),
                PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
            ::LocalFree(descriptor);
            wil::unique_hfile pipe{ rawPipe };
            if (!pipe)
            {
                Log(L"could not create the pipe (" + std::to_wstring(::GetLastError()) + L")");
                // Retry later (e.g. a previous instance is still shutting down).
                if (::WaitForSingleObject(m_stop.get(), 5000) == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            OVERLAPPED overlapped{};
            overlapped.hEvent = connected.get();
            connected.ResetEvent();
            bool ready = false;
            if (::ConnectNamedPipe(pipe.get(), &overlapped))
            {
                ready = true;
            }
            else
            {
                DWORD const error = ::GetLastError();
                if (error == ERROR_PIPE_CONNECTED)
                {
                    ready = true;
                }
                else if (error == ERROR_IO_PENDING)
                {
                    HANDLE handles[] = { connected.get(), m_stop.get() };
                    if (::WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0)
                    {
                        ::CancelIoEx(pipe.get(), &overlapped);
                        DWORD ignored = 0;
                        ::GetOverlappedResult(pipe.get(), &overlapped, &ignored, TRUE);
                        break;
                    }
                    DWORD ignored = 0;
                    ready = ::GetOverlappedResult(pipe.get(), &overlapped, &ignored, FALSE) != FALSE;
                }
            }
            if (!ready)
            {
                continue;
            }
            // Keep the listener responsive: each connected client has a bounded worker. Pipe I/O
            // itself has finite deadlines, and shutdown joins every worker after cancelling I/O.
            while (!m_stopping && m_activeWorkers.load() >= 8)
            {
                if (::WaitForSingleObject(m_stop.get(), 50) == WAIT_OBJECT_0) break;
            }
            if (m_stopping) break;
            ++m_activeWorkers;
            std::scoped_lock lock{ m_workersMutex };
            m_workers.emplace_back([this, pipe = std::move(pipe)]() mutable {
                try
                {
                    Serve(pipe.get());
                    ::FlushFileBuffers(pipe.get());
                    ::DisconnectNamedPipe(pipe.get());
                }
                catch (...)
                {
                    Log(L"request failed");
                }
                --m_activeWorkers;
            });
        }
        winrt::uninit_apartment();
    }

    void Server::Serve(HANDLE pipe)
    {
        HANDLE const stop = m_stop.get();
        auto transfer = [pipe, stop](bool write, std::uint8_t* data, std::size_t size) {
            return TransferAsync(pipe, stop, write, data, size);
        };
        auto request = ReadFrame(transfer, MaxRequestSize);
        if (!request)
        {
            return;
        }
        Result result;
        if (auto command = DeserializeCommand(*request))
        {
            result = Dispatch(*command);
        }
        else
        {
            result.exitCode = UsageError;
            result.error = Strings::Get(L"Script_BadRequest");
        }
        if (result.payload.bytes.size() > MaxPayloadSize)
        {
            result = Result{ NotFound, Strings::Get(L"Script_TooLarge"), {} };
        }
        const std::string header = SerializeResultHeader(result);
        if (WriteFrame(transfer, AsBytes(header)))
        {
            WriteFrame(transfer, result.payload.bytes);
        }
    }

    Result Server::Dispatch(Command const& command)
    {
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        auto handler = m_handler;
        bool const queued = m_dispatcher && m_dispatcher.TryEnqueue([promise, handler, command] {
            Result result;
            try
            {
                result = handler(command);
            }
            catch (...)
            {
                result = Result{ Unavailable, Strings::Get(L"Script_CouldNotRun"), {} };
            }
            promise->set_value(std::move(result));
        });
        if (!queued)
        {
            return Result{ Unavailable, Strings::Get(L"Script_Quitting"), {} };
        }
        while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        {
            if (m_stopping)
            {
                return Result{ Unavailable, Strings::Get(L"Script_Quitting"), {} };
            }
        }
        return future.get();
    }

    // ---- Client ----

    int PrintUsage()
    {
        WriteText(STD_OUTPUT_HANDLE, Usage());
        return Success;
    }

    int PrintUsageError(std::wstring const& error)
    {
        return Fail(UsageError, Strings::Format(L"Script_SeeHelp", error));
    }

    int RunClient(Command const& command)
    {
        if (command.verb == Verb::Help)
        {
            return PrintUsage();
        }

        // Lets Locket bring the Clear confirmation to the front and paste into the caller's window.
        ::AllowSetForegroundWindow(ASFW_ANY);

        int exitCode = Unavailable;
        std::wstring error;
        wil::unique_hfile pipe = Connect(exitCode, error);
        if (!pipe)
        {
            return Fail(exitCode, error);
        }

        if (!IsExpectedServer(pipe.get()))
        {
            return Fail(Unavailable, Strings::Get(L"Script_NotResponding"));
        }

        HANDLE const handle = pipe.get();
        auto transfer = [handle](bool write, std::uint8_t* data, std::size_t size) {
            return TransferSync(handle, write, data, size);
        };
        const std::string request = SerializeCommand(command);
        if (!WriteFrame(transfer, AsBytes(request)))
        {
            return Fail(Unavailable, Strings::Get(L"Script_SendFailed"));
        }
        auto header = ReadFrame(transfer, MaxRequestSize);
        auto payload = header ? ReadFrame(transfer, MaxPayloadSize) : std::nullopt;
        if (!header || !payload)
        {
            return Fail(Unavailable, Strings::Get(L"Script_NoAnswer"));
        }

        JsonObject json{ nullptr };
        if (!JsonObject::TryParse(AsWide(*header), json) || !json)
        {
            return Fail(Unavailable, Strings::Get(L"Script_InvalidAnswer"));
        }
        int const code = static_cast<int>(json.GetNamedNumber(L"exitCode", Unavailable));
        if (code != Success)
        {
            auto const error = json.GetNamedString(L"error", L"");
            return Fail(code, error.empty() ? Strings::Get(L"Script_Failed") : std::wstring{ error });
        }
        Format const format = FormatFromName(json.GetNamedString(L"format", L"text")).value_or(Format::Text);

        if (command.verb == Verb::Get && !command.output.empty())
        {
            std::error_code ec;
            auto path = std::filesystem::absolute(std::filesystem::path{ command.output }, ec);
            if (ec)
            {
                path = command.output;
            }
            wil::unique_hfile file{ ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
            if (!file || !TransferSync(file.get(), true, payload->data(), payload->size()))
            {
                if (file)
                {
                    file.reset();
                    ::DeleteFileW(path.c_str());
                }
                return Fail(UsageError, Strings::Format(L"Script_WriteFailed", path.wstring()));
            }
            WriteText(STD_OUTPUT_HANDLE, path.wstring());
            return Success;
        }

        if (command.verb == Verb::Get || command.verb == Verb::Select)
        {
            if (format == Format::Image)
            {
                const std::string encoded = Base64(*payload);
                Write(STD_OUTPUT_HANDLE, AsBytes(encoded), true);
            }
            else
            {
                Write(STD_OUTPUT_HANDLE, *payload, true);
            }
        }
        return Success;
    }
}
