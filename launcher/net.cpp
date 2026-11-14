// Endless Keeper launcher: the release list from GitHub, downloads, and installing an update.
#include "net.h"
#include "json.h"
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

std::wstring widen(const std::string &s)
{
    if (s.empty())
        return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::vector<int> version_numbers(const std::string &text)
{
    std::vector<int> v;
    for (size_t i = 0; i < text.size();)
    {
        if (isdigit((unsigned char)text[i]))
        {
            int n = 0;
            while ((i < text.size()) && isdigit((unsigned char)text[i]))
                n = n * 10 + (text[i++] - '0');
            v.push_back(n);
        } else
            i++;
    }
    return v;
}

bool version_newer(const std::vector<int> &a, const std::vector<int> &b)
{
    for (size_t i = 0; i < std::max(a.size(), b.size()); i++)
    {
        const int x = (i < a.size()) ? a[i] : 0, y = (i < b.size()) ? b[i] : 0;
        if (x != y)
            return x > y;
    }
    return false;
}

namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h != nullptr) WinHttpCloseHandle(h); }
};

std::string win_error(const char *what)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "%s (error %lu)", what, (unsigned long)GetLastError());
    return buf;
}

/** GET; every piece of the answer goes to sink (false from it stops). total: the Content-Length, or 0. */
bool http_request(const std::string &url, const std::function<bool(const char *, size_t, long long)> &sink, std::string &why)
{
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc))
    {
        why = "bad address";
        return false;
    }
    Handle session, connect, request;
    session.h = WinHttpOpen(L"EndlessKeeperLauncher", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.h == nullptr)
        session.h = WinHttpOpen(L"EndlessKeeperLauncher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session.h == nullptr)
    {
        why = win_error("no internet session");
        return false;
    }
    WinHttpSetTimeouts(session.h, 8000, 8000, 15000, 30000);
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (connect.h == nullptr)
    {
        why = win_error("can't connect");
        return false;
    }
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (request.h == nullptr)
    {
        why = win_error("can't open the request");
        return false;
    }
    static const wchar_t headers[] = L"Accept: application/vnd.github+json, application/octet-stream;q=0.9, */*;q=0.5\r\n";
    if (!WinHttpSendRequest(request.h, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.h, nullptr))
    {
        why = win_error("no answer");
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
        WINHTTP_NO_HEADER_INDEX);
    if (status != 200)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "the server answered %lu", (unsigned long)status);
        why = buf;
        return false;
    }
    long long total = 0;
    {
        wchar_t text[32];
        DWORD tl = sizeof(text);
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, text, &tl, WINHTTP_NO_HEADER_INDEX))
            total = _wtoi64(text);
    }
    std::vector<char> buf(64 * 1024);
    for (;;)
    {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buf.data(), (DWORD)buf.size(), &got))
        {
            why = win_error("the download broke off");
            return false;
        }
        if (got == 0)
            break;
        if (!sink(buf.data(), got, total))
        {
            if (why.empty())
                why = "stopped";
            return false;
        }
    }
    return true;
}

bool sha256_file(const std::wstring &file, std::string &hex)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    FILE *f = _wfopen(file.c_str(), L"rb");
    if (f == nullptr)
        return false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0)
    {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            std::vector<unsigned char> buf(256 * 1024);
            size_t n;
            ok = true;
            while (ok && ((n = fread(buf.data(), 1, buf.size(), f)) > 0))
                ok = BCryptHashData(hash, buf.data(), (ULONG)n, 0) == 0;
            unsigned char out[32];
            ok = ok && (BCryptFinishHash(hash, out, 32, 0) == 0);
            if (ok)
            {
                char text[65];
                for (int i = 0; i < 32; i++)
                    snprintf(text + i * 2, 3, "%02x", out[i]);
                hex = text;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    fclose(f);
    return ok;
}

/** Runs a program without a window and waits for it. Its exit code, or -1. */
int run_hidden(const std::wstring &command, const std::wstring &cwd)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    std::wstring cmd = command;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

std::wstring tar_exe()
{
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    /* a 32-bit program's System32 is SysWOW64: the 64-bit tar is under Sysnative */
    for (const wchar_t *sub : {L"\\Sysnative\\tar.exe", L"\\System32\\tar.exe"})
    {
        const std::wstring p = std::wstring(win) + sub;
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES)
            return p;
    }
    return L"";
}

bool ends_with(const std::string &s, const char *tail)
{
    const size_t n = strlen(tail);
    return (s.size() >= n) && (_stricmp(s.c_str() + s.size() - n, tail) == 0);
}

/** The patch of a release: the files to lay over an Endless Keeper folder (a zip first: every Windows can unpack one). */
const ReleaseAsset *patch_asset(const Release &rel)
{
    for (const char *tail : {"-patch.zip", ".zip", ".7z"})
        for (const ReleaseAsset &a : rel.assets)
            if (ends_with(a.name, tail) && (a.name.find("Setup") == std::string::npos))
                return &a;
    return nullptr;
}

/** What an update leaves alone: the player's settings and saves. */
bool keep_players_own(const fs::path &rel, const fs::path &dest)
{
    const std::wstring first = rel.begin()->wstring(), name = rel.filename().wstring();
    if ((_wcsicmp(first.c_str(), L"save") == 0) || (_wcsicmp(name.c_str(), L"endlesskeeper-launcher.cfg") == 0))
        return true;
    return (_wcsicmp(rel.wstring().c_str(), L"keeperfx.cfg") == 0) && fs::exists(dest);
}

} // namespace

bool http_get(const std::string &url, std::string &out, std::string &why)
{
    out.clear();
    return http_request(url, [&](const char *data, size_t n, long long) {
        out.append(data, n);
        return out.size() < 64u * 1024 * 1024;
    }, why);
}

bool http_download(const std::string &url, const std::wstring &file, const std::function<void(long long, long long)> &progress,
    const std::atomic<bool> &cancel, std::string &why)
{
    FILE *f = _wfopen(file.c_str(), L"wb");
    if (f == nullptr)
    {
        why = "can't write the download";
        return false;
    }
    long long done = 0;
    const bool ok = http_request(url, [&](const char *data, size_t n, long long total) {
        if (cancel.load() || (fwrite(data, 1, n, f) != n))
            return false;
        done += (long long)n;
        progress(done, total);
        return true;
    }, why);
    fclose(f);
    if (!ok)
        DeleteFileW(file.c_str());
    return ok;
}

bool fetch_releases(const std::string &repo, std::vector<Release> &out, std::string &why)
{
    std::string text;
    if (!http_get("https://api.github.com/repos/" + repo + "/releases?per_page=30", text, why))
        return false;
    JVal root;
    if (!json_parse(text, root) || (root.t != JVal::Arr))
    {
        why = "the release list could not be read";
        return false;
    }
    out.clear();
    for (const JVal &r : root.a)
    {
        if ((r.t != JVal::Obj) || r.boolean("draft"))
            continue;
        Release rel;
        rel.tag = r.str("tag_name");
        rel.title = r.str("name");
        if (rel.title.empty())
            rel.title = rel.tag;
        rel.url = r.str("html_url");
        rel.body = r.str("body");
        rel.date = r.str("published_at").substr(0, 10);
        rel.version = version_numbers(rel.tag);
        const JVal *assets = r.get("assets");
        if ((assets != nullptr) && (assets->t == JVal::Arr))
            for (const JVal &a : assets->a)
            {
                ReleaseAsset as;
                as.name = a.str("name");
                as.url = a.str("browser_download_url");
                as.digest = a.str("digest");
                as.size = a.inum("size");
                rel.assets.push_back(as);
            }
        out.push_back(rel);
    }
    std::stable_sort(out.begin(), out.end(), [](const Release &a, const Release &b) { return version_newer(a.version, b.version); });
    return true;
}

/** What the player calls a program of the game folder (its path under the folder, in small letters). */
static std::string program_name(const std::wstring &rel_path)
{
    const size_t slash = rel_path.find_last_of(L"\\/");
    const std::wstring file = (slash == std::wstring::npos) ? rel_path : rel_path.substr(slash + 1);
    if (file == L"keeperserver.exe")
        return "Keeper Server";
    if (file == L"keeperfx.exe")
        return "Endless Keeper (the game, or a world that Keeper Server runs)";
    if (file == L"endlessviewer.exe")
        return "Endless World Viewer";
    if (file == L"endlesskeeper.exe")
        return "Endless Keeper's launcher (another window of it)";
    return narrow(rel_path);
}

std::vector<std::string> programs_open_in(const std::wstring &game_dir)
{
    std::vector<std::string> names;
    std::wstring dir = game_dir;
    for (wchar_t &c : dir)
        c = (c == L'/') ? L'\\' : (wchar_t)towlower(c);
    while (!dir.empty() && (dir.back() == L'\\'))
        dir.pop_back();
    dir += L'\\';
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return names;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe))
    {
        if (pe.th32ProcessID == GetCurrentProcessId())
            continue;
        const HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (h == nullptr)
            continue;
        wchar_t path[1024];
        DWORD len = 1024;
        if (QueryFullProcessImageNameW(h, 0, path, &len))
        {
            std::wstring image(path, len);
            for (wchar_t &c : image)
                c = (wchar_t)towlower(c);
            if (image.compare(0, dir.size(), dir) == 0)
            {
                const std::string name = program_name(image.substr(dir.size()));
                if (std::find(names.begin(), names.end(), name) == names.end())
                    names.push_back(name);
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return names;
}

/** Waits until nothing runs out of the game folder any more, telling the player what to close. False: cancelled. */
static bool wait_until_closed(const std::wstring &game_dir, const std::function<void(const char *, float)> &progress,
    const std::atomic<bool> &cancel, std::string &why)
{
    for (int round = 0;; round++)
    {
        const std::vector<std::string> open = programs_open_in(game_dir);
        if (open.empty())
            return true;
        if (cancel.load())
        {
            why = "the update was cancelled; nothing was changed";
            return false;
        }
        if (round >= 2) /* (a program that is just closing gets a moment) */
        {
            std::string stage = "Close:";
            for (size_t i = 0; i < open.size(); i++)
                stage += ((i > 0) ? "|" : "") + open[i];
            progress(stage.c_str(), 0.0f);
        }
        Sleep(300);
    }
}

bool install_release(const Release &rel, const std::wstring &game_dir, const std::wstring &self_exe,
    const std::function<void(const char *, float)> &progress, const std::atomic<bool> &cancel, std::string &why)
{
    /* test hook EKL_TEST_PACKAGE=<file>: that patch instead of the release's download */
    const wchar_t *local_package = _wgetenv(L"EKL_TEST_PACKAGE");
    const ReleaseAsset *asset = (local_package != nullptr) ? nullptr : patch_asset(rel);
    if ((asset == nullptr) && (local_package == nullptr))
    {
        why = "this release has no patch to download";
        return false;
    }
    /* before anything is fetched: programs running out of the game folder must be closed */
    if (!wait_until_closed(game_dir, progress, cancel, why))
        return false;
    std::error_code ec;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path work = fs::path(tmp) / L"EndlessKeeperUpdate";
    fs::remove_all(work, ec);
    fs::create_directories(work / L"files", ec);
    const fs::path archive = work / ((asset != nullptr) ? widen(asset->name) : fs::path(local_package).filename().wstring());
    progress("Downloading", 0.0f);
    if (asset == nullptr)
    {
        if (!CopyFileW(local_package, archive.c_str(), FALSE))
        {
            why = "the test package could not be read";
            return false;
        }
    } else if (!http_download(asset->url, archive.wstring(), [&](long long done, long long total) {
            const long long all = (total > 0) ? total : asset->size;
            progress("Downloading", (all > 0) ? (float)((double)done / (double)all) : 0.0f);
        }, cancel, why))
        return false;
    progress("Checking", 1.0f);
    if ((asset != nullptr) && (asset->digest.rfind("sha256:", 0) == 0))
    {
        std::string hex;
        if (!sha256_file(archive.wstring(), hex) || (_stricmp(hex.c_str(), asset->digest.c_str() + 7) != 0))
        {
            why = "the download is damaged (its checksum is wrong)";
            return false;
        }
    }
    /* nothing may run out of the game folder now (something may have been started during the download) */
    if (!wait_until_closed(game_dir, progress, cancel, why))
        return false;
    /* and the game's programs must be free to write (a program started from elsewhere that holds them) */
    for (const wchar_t *exe : {L"keeperfx.exe", L"x64\\keeperfx.exe"})
    {
        const fs::path p = fs::path(game_dir) / exe;
        if (!fs::exists(p, ec))
            continue;
        const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            why = "the game (or its server) is running: close it and update again";
            return false;
        }
        CloseHandle(h);
    }
    progress("Unpacking", 1.0f);
    const std::wstring tar = tar_exe();
    if (tar.empty() || (run_hidden(L"\"" + tar + L"\" -xf \"" + archive.wstring() + L"\" -C \"" + (work / L"files").wstring() + L"\"", L"") != 0))
    {
        why = "the update could not be unpacked on this computer";
        return false;
    }
    /* a package with one folder at its top: the files are in it */
    fs::path root = work / L"files";
    {
        int dirs = 0, files = 0;
        fs::path only;
        for (const fs::directory_entry &e : fs::directory_iterator(root, ec))
        {
            if (e.is_directory())
            {
                dirs++;
                only = e.path();
            } else
                files++;
        }
        if ((dirs == 1) && (files == 0))
            root = only;
    }
    if (!fs::exists(root / L"keeperfx.exe", ec))
    {
        why = "the update doesn't look like an Endless Keeper patch";
        return false;
    }
    progress("Installing", 1.0f);
    std::vector<fs::path> files;
    for (const fs::directory_entry &e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file())
            files.push_back(e.path());
    bool self_moved = false;
    const fs::path self(self_exe), self_old = fs::path(self_exe + L".old");
    size_t n = 0;
    for (const fs::path &src : files)
    {
        const fs::path rel_path = fs::relative(src, root, ec), dest = fs::path(game_dir) / rel_path;
        progress("Installing", (float)(++n) / (float)files.size());
        if (keep_players_own(rel_path, dest))
            continue;
        fs::create_directories(dest.parent_path(), ec);
        if (fs::equivalent(dest, self, ec) && !self_moved)
        {
            /* a running program can't be written over, but it can be renamed */
            DeleteFileW(self_old.c_str());
            if (!MoveFileExW(self.c_str(), self_old.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                why = "the launcher could not make room for its new version";
                return false;
            }
            self_moved = true;
        }
        /* (Windows' own copy: the library's would not write over a file that is there) */
        SetFileAttributesW(dest.c_str(), FILE_ATTRIBUTE_NORMAL);
        bool copied = CopyFileW(src.c_str(), dest.c_str(), FALSE) != 0;
        if (!copied)
        {
            /* in use (a DLL this launcher has loaded): renamed out of the way like the launcher itself; the launcher's
               next start removes what is listed in endlesskeeper-launcher.old */
            const fs::path aside = fs::path(dest.wstring() + L".old");
            DeleteFileW(aside.c_str());
            if (MoveFileExW(dest.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
            {
                copied = CopyFileW(src.c_str(), dest.c_str(), FALSE) != 0;
                if (copied)
                {
                    FILE *list = _wfopen((fs::path(game_dir) / L"endlesskeeper-launcher.old").c_str(), L"a");
                    if (list != nullptr)
                    {
                        fputs(narrow(fs::relative(aside, game_dir, ec).wstring()).c_str(), list);
                        fputc(10, list);
                        fclose(list);
                    }
                } else
                    MoveFileExW(aside.c_str(), dest.c_str(), 0);
            }
        }
        if (!copied)
        {
            const unsigned long err = GetLastError();
            if (self_moved && !fs::exists(self))
                MoveFileExW(self_old.c_str(), self.c_str(), 0);
            why = "could not write " + narrow(rel_path.wstring()) + " (error " + std::to_string(err) + ")";
            return false;
        }
    }
    fs::remove_all(work, ec);
    return true;
}
