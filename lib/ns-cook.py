#!/usr/bin/env python3
"""Build-time texture cooking. No Python or encoder is needed by the app."""
import argparse
import fnmatch
import hashlib
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile

PLATFORMS = {"darwin": "macos", "win": "windows", "iphoneos": "ios",
             "iphonesimulator": "ios", "macosx": "macos", "xros": "visionos",
             "xrsimulator": "visionos", "android": "android", "linux": "linux",
             "ios": "ios", "macos": "macos", "windows": "windows", "visionos": "visionos"}
FORMATS = {"astc_4x4": (68, 4, 16), "astc_6x6": (70, 6, 16),
           "bc1": (45, 4, 8), "bc3": (47, 4, 16)}
EXTENSIONS = {".png", ".jpg", ".jpeg", ".webp", ".tga", ".bmp"}


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def rule_for(manifest, platform):
    rules = manifest.get("cook", {})
    if not isinstance(rules, dict):
        raise ValueError("cook must contain [cook.<platform>] tables")
    for name, rule in rules.items():
        if name not in set(PLATFORMS.values()) or not isinstance(rule, dict):
            raise ValueError(f"unknown cook platform: {name}")
        unknown = set(rule) - {"format", "max_size", "include", "exclude", "quality", "srgb"}
        if unknown:
            raise ValueError(f"unknown cook options for {name}: {sorted(unknown)}")
        fmt = rule.get("format")
        if fmt not in FORMATS:
            raise ValueError(f"cook.{name}.format must be one of {', '.join(FORMATS)}")
        if fmt.startswith("astc") and name not in {"ios", "macos", "visionos", "android"}:
            raise ValueError(f"ASTC is not supported for cook.{name}; use bc1 or bc3")
        size = rule.get("max_size", 512 if name in {"ios", "android", "visionos"} else 0)
        if type(size) is not int or size < 0 or size > 16384:
            raise ValueError(f"cook.{name}.max_size must be 0..16384 (0 means unlimited)")
        for key in ("include", "exclude"):
            if key in rule and (not isinstance(rule[key], list) or
                                not all(isinstance(v, str) for v in rule[key])):
                raise ValueError(f"cook.{name}.{key} must be an array of glob strings")
        if rule.get("quality", "medium") not in {"fastest", "fast", "medium", "thorough", "exhaustive"}:
            raise ValueError(f"invalid cook.{name}.quality")
        if type(rule.get("srgb", False)) is not bool:
            raise ValueError(f"cook.{name}.srgb must be a boolean")
        # BC1 has no sRGB enum in the current ABI.
        if fmt == "bc1" and rule.get("srgb", False):
            raise ValueError("bc1 srgb is not supported; use bc3")
    return rules.get(platform)


def asset_paths(manifest, root):
    paths = manifest.get("assets")
    if paths is None:
        paths = ["assets", str(Path(manifest.get("source", "src")) / "assets")]
    for value in paths:
        path = Path(value)
        if path.is_absolute() or ".." in path.parts:
            raise ValueError(f"asset path must stay inside the project: {value}")
        source = root / path
        if source.is_file():
            yield path
        elif source.is_dir():
            for child in sorted(source.rglob("*")):
                if child.is_file():
                    yield child.relative_to(root)


def cook(root, output, platform):
    root, output = Path(root).resolve(), Path(output).resolve()
    if (output.is_relative_to(root) and not output.is_relative_to(root / "bin")) or root.is_relative_to(output):
        raise ValueError("cook output must be a separate generated resource directory")
    text = (root / "ns.mod").read_text()
    if "[cook." not in text:
        return 0
    if sys.version_info < (3, 11):
        raise ValueError("texture cooking requires Python 3.11+; set NS_COOK_PYTHON to that interpreter")
    import tomllib
    manifest = tomllib.loads(text)
    platform = PLATFORMS.get(platform, platform)
    if platform not in set(PLATFORMS.values()):
        raise ValueError(f"unknown cook platform: {platform}")
    rule = rule_for(manifest, platform)
    if rule is None:
        return 0
    from PIL import Image
    fmt, block, block_bytes = FORMATS[rule["format"]]
    srgb = rule.get("srgb", False)
    if srgb:
        fmt += 1
    limit = rule.get("max_size", 512 if platform in {"ios", "android", "visionos"} else 0)
    encoder = None
    if rule["format"].startswith("astc"):
        bundled = Path(__file__).resolve().parent.parent / "bin" / "astcenc"
        encoder = os.environ.get("NS_ASTCENC") or shutil.which("astcenc") or shutil.which("astcenc-neon")
        if not encoder and bundled.is_file():
            encoder = str(bundled)
        if encoder:
            encoder = shutil.which(encoder) or str(Path(encoder).resolve())
        if not encoder:
            raise ValueError("ASTC cooking requires astcenc on PATH, or NS_ASTCENC=/path/to/astcenc")
    count = 0
    for relative in asset_paths(manifest, root):
        name = relative.as_posix()
        if relative.suffix.lower() not in EXTENSIONS or not matches(name, rule.get("include", ["*"])) or matches(name, rule.get("exclude", [])):
            (output / (name + ".nstex")).unlink(missing_ok=True)
            continue
        source = root / relative
        destination = output / (name + ".nstex")
        original = output / relative
        if original.resolve() == source.resolve() or destination.resolve() == source.resolve():
            raise ValueError("cook output would overwrite a source asset")
        fingerprint = hashlib.sha256(source.read_bytes() + repr(sorted(rule.items())).encode() +
                                     Path(__file__).read_bytes())
        if encoder:
            info = Path(encoder).stat()
            fingerprint.update(f"{encoder}:{info.st_size}:{info.st_mtime_ns}".encode())
        else:
            import PIL
            fingerprint.update(PIL.__version__.encode())
        cached = root / "bin" / ".ns-cook" / platform / (fingerprint.hexdigest() + ".nstex")
        if cached.is_file():
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cached, destination)
            original.unlink(missing_ok=True)
            count += 1
            continue
        with Image.open(source) as image:
            # Data images (16-bit PNG etc.) need explicit, lossless handling.
            if image.mode in {"I", "F", "I;16", "I;16B", "I;16L"}:
                raise ValueError(f"{name}: data image must be excluded from texture cooking")
            original_width, original_height = image.size
            image = image.convert("RGBA")
            if limit and max(image.size) > limit:
                width, height = image.size
                scale = limit / max(width, height)
                image = image.resize((max(1, int(width * scale)), max(1, int(height * scale))), Image.Resampling.LANCZOS)
            width, height = image.size
            with tempfile.TemporaryDirectory(prefix="ns-cook-") as temp:
                temp = Path(temp)
                if encoder:
                    image.save(temp / "input.png")
                    subprocess.run([encoder, "-cs" if srgb else "-cl", str(temp / "input.png"),
                                    str(temp / "output.astc"), f"{block}x{block}",
                                    "-" + rule.get("quality", "medium"), "-silent"], check=True)
                    encoded = (temp / "output.astc").read_bytes()
                    if encoded[:4] != b"\x13\xab\xa1\x5c" or encoded[4:7] != bytes([block, block, 1]):
                        raise ValueError(f"{name}: invalid ASTC encoder output")
                    payload = encoded[16:]
                else:
                    image.save(temp / "output.dds", pixel_format="DXT1" if rule["format"] == "bc1" else "DXT5")
                    encoded = (temp / "output.dds").read_bytes()
                    if encoded[:4] != b"DDS " or encoded[84:88] != (b"DXT1" if rule["format"] == "bc1" else b"DXT5"):
                        raise ValueError(f"{name}: unsupported DDS encoder output (requires Pillow >= 11.2.1)")
                    payload = encoded[128:]
                expected = ((width + block - 1) // block) * ((height + block - 1) // block) * block_bytes
                if len(payload) != expected:
                    raise ValueError(f"{name}: encoder payload size mismatch")
                data = struct.pack("<4s7I", b"NSTX", 1, fmt, width, height, original_width, original_height, len(payload)) + payload
                destination.parent.mkdir(parents=True, exist_ok=True)
                # Atomic replacement; only remove the packaged source after success.
                staging = destination.with_suffix(".nstex.tmp")
                staging.write_bytes(data)
                staging.replace(destination)
                cached.parent.mkdir(parents=True, exist_ok=True)
                with tempfile.NamedTemporaryFile(dir=cached.parent, delete=False) as cache_file:
                    cache_file.write(data)
                    cache_temp = Path(cache_file.name)
                cache_temp.replace(cached)
                original.unlink(missing_ok=True)
                count += 1
                print(f"cook {platform}: {name} -> {width}x{height} {rule['format']} ({len(payload)} bytes)")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=".")
    parser.add_argument("--platform", required=True)
    parser.add_argument("--output", required=True, help="already copied bundle resource directory")
    args = parser.parse_args()
    try:
        cook(args.root, args.output, args.platform)
    except (ValueError, OSError, ImportError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"ns cook: {error}\n")


if __name__ == "__main__":
    main()
