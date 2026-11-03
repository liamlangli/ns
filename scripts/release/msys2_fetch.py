#!/usr/bin/env python3
# Download MSYS2 ucrt64 packages and their dependencies and unpack them into
# a root, so a Linux cross toolchain finds /ucrt64/include and /ucrt64/lib the
# way an MSYS2 shell does.
#
#   msys2_fetch.py <root> <package>...
import io
import subprocess
import sys
import tarfile
import time
import urllib.request

REPO = "https://repo.msys2.org/mingw/ucrt64"
PREFIX = "mingw-w64-ucrt-x86_64-"


def fetch(url, attempts=5):
    for attempt in range(attempts):
        try:
            with urllib.request.urlopen(url, timeout=60) as response:
                return response.read()
        except OSError:
            if attempt + 1 == attempts:
                raise
            time.sleep(2 * (attempt + 1))


def unzstd(data):
    return subprocess.run(["zstd", "-dc"], input=data, stdout=subprocess.PIPE, check=True).stdout


def bare(dependency):
    for op in ("<", ">", "="):
        dependency = dependency.split(op)[0]
    return dependency.strip()


def read_db():
    packages, provides = {}, {}
    with tarfile.open(fileobj=io.BytesIO(unzstd(fetch(f"{REPO}/ucrt64.db")))) as db:
        for member in db.getmembers():
            if not member.name.endswith("/desc"):
                continue
            fields, key = {}, None
            for line in db.extractfile(member).read().decode().splitlines():
                if line.startswith("%") and line.endswith("%"):
                    key = line.strip("%")
                    fields[key] = []
                elif line and key:
                    fields[key].append(line)
            name = fields["NAME"][0]
            packages[name] = fields
            for provided in fields.get("PROVIDES", []):
                provides[bare(provided)] = name
    return packages, provides


def main():
    root, wanted = sys.argv[1], sys.argv[2:]
    packages, provides = read_db()
    queue = [PREFIX + name for name in wanted]
    seen = set()
    while queue:
        name = queue.pop()
        name = name if name in packages else provides.get(name, name)
        if name in seen:
            continue
        if name not in packages:
            sys.exit(f"msys2_fetch: unknown package {name}")
        seen.add(name)
        queue.extend(bare(d) for d in packages[name].get("DEPENDS", []))
    for name in sorted(seen):
        filename = packages[name]["FILENAME"][0]
        print(f"msys2_fetch: {filename}", flush=True)
        archive = unzstd(fetch(f"{REPO}/{filename}"))
        with tarfile.open(fileobj=io.BytesIO(archive)) as package:
            package.extractall(root, members=[m for m in package.getmembers() if m.name.startswith("ucrt64/")])


main()
