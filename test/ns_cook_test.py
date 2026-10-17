"""Exercise real encoders and packaging invariants without starting an app."""
import importlib.util
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
import sys
sys.dont_write_bytecode = True

spec = importlib.util.spec_from_file_location("cook", Path(__file__).resolve().parents[1] / "lib/ns-cook.py")
cook = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cook)


class CookTest(unittest.TestCase):
    def setUp(self):
        try:
            import tomllib
            from PIL import Image
        except ImportError:
            self.skipTest("texture cooking requires Python 3.11+ and Pillow")
        self.Image = Image
        self.temp = tempfile.TemporaryDirectory(prefix="ns-cook-test ' ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "res").mkdir()
        self.Image.new("RGBA", (1025, 513), (60, 90, 140, 128)).save(self.root / "res/colour.png")
        self.Image.new("I;16", (16, 16)).save(self.root / "res/data.png")
        self.original = (self.root / "res/colour.png").read_bytes()
        self.output = self.root / "bin/resources"

    def manifest(self, platform="windows", fmt="bc3", extra=""):
        (self.root / "ns.mod").write_text(f'assets = ["res"]\n[cook.{platform}]\nformat = "{fmt}"\nmax_size = 512\nexclude = ["res/data.png"]\n{extra}')
        shutil.copytree(self.root / "res", self.output / "res", dirs_exist_ok=True)

    def check_output(self, fmt, block, block_bytes):
        data = (self.output / "res/colour.png.nstex").read_bytes()
        self.assertEqual(struct.unpack("<4s7I", data[:32]),
                         (b"NSTX", 1, fmt, 512, 256, 1025, 513, len(data) - 32))
        self.assertEqual(len(data) - 32, ((512 + block - 1) // block) * ((256 + block - 1) // block) * block_bytes)
        self.assertFalse((self.output / "res/colour.png").exists())
        self.assertTrue((self.output / "res/data.png").exists())
        self.assertEqual((self.root / "res/colour.png").read_bytes(), self.original)

    def test_bc3_and_cache(self):
        self.manifest()
        self.assertEqual(cook.cook(self.root, self.output, "windows"), 1)
        self.check_output(47, 4, 16)
        first = (self.output / "res/colour.png.nstex").read_bytes()
        shutil.copyfile(self.root / "res/colour.png", self.output / "res/colour.png")
        cook.cook(self.root, self.output, "windows")
        self.assertEqual(first, (self.output / "res/colour.png.nstex").read_bytes())
        self.check_output(47, 4, 16)

    def test_resize_keeps_transparent_colour(self):
        # White strokes on transparent white: shrinking must not blacken the
        # transparent texels, or the filtered strokes grow a dark fringe.
        image = self.Image.new("RGBA", (64, 64), (255, 255, 255, 0))
        for x in range(20, 44):
            for y in range(20, 44):
                image.putpixel((x, y), (255, 255, 255, 255))
        image.save(self.root / "res/colour.png")
        self.manifest()
        (self.root / "ns.mod").write_text('assets = ["res"]\n[cook.windows]\nformat = "bc3"\nmax_size = 32\nexclude = ["res/data.png"]\n')
        self.assertEqual(cook.cook(self.root, self.output, "windows"), 1)
        data = (self.output / "res/colour.png.nstex").read_bytes()
        self.assertEqual(struct.unpack("<4s7I", data[:32])[3:5], (32, 32))
        # The first BC3 block is fully transparent: its alpha endpoints are
        # 0 and both its colour endpoints are still white.
        block = data[32:48]
        self.assertEqual(max(block[0], block[1]), 0)
        self.assertEqual(struct.unpack("<HH", block[8:12]), (0xFFFF, 0xFFFF))

    def test_astc_real_encoder(self):
        import os
        bundled = Path(cook.__file__).resolve().parent.parent / "bin/astcenc"
        if not (os.environ.get("NS_ASTCENC") or shutil.which("astcenc") or shutil.which("astcenc-neon") or bundled.is_file()):
            self.skipTest("astcenc is not installed")
        for fmt, block, number in (("astc_4x4", 4, 69), ("astc_6x6", 6, 71)):
            self.manifest("ios", fmt, 'srgb = true\nquality = "fastest"\n')
            cook.cook(self.root, self.output, "iphonesimulator")
            self.check_output(number, block, 16)

    def test_validation_and_source_protection(self):
        self.manifest()
        with self.assertRaises(ValueError):
            cook.cook(self.root, self.root, "windows")
        with self.assertRaises(ValueError):
            cook.rule_for({"cook": {"windows": {"format": "astc_6x6"}}}, "windows")
        with self.assertRaises(ValueError):
            cook.rule_for({"cook": {"ios": {"format": "astc_6x6", "max_size": -1}}}, "ios")
        self.assertEqual(cook.cook(self.root, self.output, "macos"), 0)
        self.assertTrue((self.output / "res/colour.png").exists())

    def test_cli_with_quoted_paths_and_standalone_patch_asset(self):
        import subprocess
        ns = Path(cook.__file__).resolve().parent.parent / "bin/ns"
        self.manifest()
        subprocess.run([str(ns), "cook", str(self.root), "--platform", "windows",
                        "--output", str(self.output)], check=True, capture_output=True)
        self.check_output(47, 4, 16)
        (self.root / "src").mkdir()
        (self.root / "src/main.ns").write_text('fn main() {}\n')
        (self.root / "ns.mod").write_text('schema = "ns.mod/v2"\nname = "cook-test"\ntype = "app"\nsource = "src"\nentry = "main.ns"\nassets = ["res/colour.png"]\n[cook.windows]\nformat = "bc3"\n')
        result = subprocess.run([str(ns), "patch", "--cook-platform", "windows", str(self.root)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue((self.root / "bin/cook-test_patch/cook-test.nsapp").is_file())
        self.assertFalse((self.root / "bin/cook-test_patch/cook-staging").exists())
        self.assertEqual((self.root / "res/colour.png").read_bytes(), self.original)

    def test_source_and_rules_invalidate_cache(self):
        self.manifest()
        cook.cook(self.root, self.output, "windows")
        self.manifest("windows", "bc1")
        cook.cook(self.root, self.output, "windows")
        self.check_output(45, 4, 8)
        self.Image.new("RGBA", (8, 8), (255, 0, 0, 255)).save(self.root / "res/colour.png")
        cook.cook(self.root, self.output, "windows")
        data = (self.output / "res/colour.png.nstex").read_bytes()
        self.assertEqual(struct.unpack("<II", data[12:20]), (8, 8))


if __name__ == "__main__":
    unittest.main()
