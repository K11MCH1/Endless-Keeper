"""Downloads the Windows build's CMake dependencies into out/deps/<name>.tar.gz before CMake runs, with retries.

GitHub release downloads sometimes fail for minutes at a time, and CMake's file(DOWNLOAD) tries only once; kfx_fetch
(build/cmake/modules/Dependencies.cmake) skips the download when the archive is already there. The list and the
versions are read from that file, so they follow KeeperFX. --dry-run only prints what it would fetch.
"""
import os
import re
import subprocess
import sys

CMAKE = "build/cmake/modules/Dependencies.cmake"


def main():
    text = open(CMAKE, encoding="utf-8").read()
    start = text.index("if(WIN32)")
    branch = text[start:]
    end = re.search(r"^else\(\)", branch, re.M)  # the Windows branch ends at its own (unindented) else()
    if end:
        branch = branch[:end.start()]
    values = {}
    for name, value in re.findall(r'set\((\w+)\s+"?([^\s")]+)"?\)', text[:start] + branch):
        values.setdefault(name, value)

    def expand(s):
        return re.sub(r"\$\{(\w+)\}", lambda m: values.get(m.group(1), m.group(0)), s)

    deps = [(name, expand(url)) for name, url in re.findall(r'kfx_fetch\((\w+)\s+"([^"]+)"', branch)]
    if not deps:
        sys.exit("no kfx_fetch found in " + CMAKE)
    if "--dry-run" in sys.argv:
        for name, url in deps:
            print(name, url)
        return
    os.makedirs("out/deps", exist_ok=True)
    for name, url in deps:
        dest = os.path.join("out", "deps", name + ".tar.gz")
        if os.path.exists(dest):
            continue
        r = subprocess.run(["curl", "-fsSL", "--retry", "12", "--retry-delay", "20", "--retry-all-errors",
                            "-o", dest + ".part", url])
        if r.returncode != 0:
            sys.exit("could not download " + url)
        os.replace(dest + ".part", dest)
        print("fetched", name, os.path.getsize(dest), "bytes")


if __name__ == "__main__":
    main()
