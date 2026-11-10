// Endless Keeper launcher: the release list from GitHub, downloads, and installing an update.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <vector>

struct ReleaseAsset {
    std::string name, url, digest; /* digest: "sha256:<hex>" when GitHub gives one */
    long long size = 0;
};

struct Release {
    std::string tag, title, url, body, date;
    std::vector<ReleaseAsset> assets;
    std::vector<int> version; /* 1.4.0-6.2 -> {1, 4, 0, 6, 2} */
};

std::wstring widen(const std::string &s);
std::string narrow(const std::wstring &s);

/** The numbers of a version text or tag ("v1.4.0-6.2-alpha" -> 1 4 0 6 2). */
std::vector<int> version_numbers(const std::string &text);
/** Is a newer than b? */
bool version_newer(const std::vector<int> &a, const std::vector<int> &b);

/** GET into memory. False with why. */
bool http_get(const std::string &url, std::string &out, std::string &why);
/** GET into a file; progress gets bytes so far and the total (0: unknown). */
bool http_download(const std::string &url, const std::wstring &file, const std::function<void(long long, long long)> &progress,
    const std::atomic<bool> &cancel, std::string &why);
/** The published releases of the repository ("owner/name"), newest first. */
bool fetch_releases(const std::string &repo, std::vector<Release> &out, std::string &why);
/** Downloads the release's patch, checks it, unpacks it over the game folder (settings and saves are kept) and leaves the
 *  running launcher renamed to <name>.old for the new one. stage: what it is doing. False with why. */
bool install_release(const Release &rel, const std::wstring &game_dir, const std::wstring &self_exe,
    const std::function<void(const char *stage, float fraction)> &progress, const std::atomic<bool> &cancel, std::string &why);
