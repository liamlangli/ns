#!/usr/bin/env python3
"""Fetch the four original IDX files, verify checksums, and decompress atomically."""
import gzip
import hashlib
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1] / "bin" / "mnist"
BASE = "https://storage.googleapis.com/cvdf-datasets/mnist/"
FILES = {
    "train-images-idx3-ubyte": ("f68b3c2dcbeaaa9fbdd348bbdeb94873", 47040016),
    "train-labels-idx1-ubyte": ("d53e105ee54ea40749a09fcbcd1e9432", 60008),
    "t10k-images-idx3-ubyte": ("9fb629c4189551a2d022fa330f9573f3", 7840016),
    "t10k-labels-idx1-ubyte": ("ec29112dd5afa0611ce80d1b7f02629c", 10008),
}


def main():
    ROOT.mkdir(parents=True, exist_ok=True)
    for name, (digest, size) in FILES.items():
        print(f"Downloading {name}...", flush=True)
        with urllib.request.urlopen(BASE + name + ".gz", timeout=120) as response:
            compressed = response.read()
        if hashlib.md5(compressed).hexdigest() != digest:
            raise ValueError(f"Checksum mismatch: {name}")
        data = gzip.decompress(compressed)
        if len(data) != size:
            raise ValueError(f"Unexpected uncompressed size: {name}")
        destination = ROOT / name
        temporary = destination.with_suffix(".tmp")
        temporary.write_bytes(data)
        temporary.replace(destination)
    print(f"MNIST ready: {ROOT}")


if __name__ == "__main__":
    main()
