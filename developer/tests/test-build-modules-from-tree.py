#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host boundary tests using fake build tools; not an ARM64 build or ABI test."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


KIT = Path(__file__).resolve().parents[1]
RELEASE = "6.18.29-test"
COMPILER = "Alpine clang version 22.1.3"
OPTIONS = ("ARM64", "MODULES", "CC_IS_CLANG", "LD_IS_LLD", "CFI")
DIRECTORIES = {
    "ov8856-standard-balanced-fps": "ov8856",
    "ov02a10-standard-fps-range": "ov02a10",
    "dw9768-upstream": "dw9768",
    "mt8183-seninf-dual-highres": "mtk_seninf",
    "mt8183-p1-public": "mt8183_p1",
}


class BuildBoundaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="duet-build-boundary-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.kernel = self.root / "kernel"
        self.output = self.root / "output"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.test_kit = self.root / "kit"
        (self.test_kit / "scripts").mkdir(parents=True)
        self.script = self.test_kit / "scripts/build-modules-from-tree.sh"
        shutil.copyfile(KIT / "scripts/build-modules-from-tree.sh", self.script)
        for directory, name in DIRECTORIES.items():
            dest = self.test_kit / "modules" / directory
            dest.mkdir(parents=True)
            (dest / "Makefile").write_text(f"obj-m := {name}.o\n")
            (dest / f"{name}.c").write_text("/* Fake source for host boundary tests. */\n")
        config = "".join(f"CONFIG_{name}=y\n" for name in OPTIONS)
        config += f'CONFIG_CC_VERSION_TEXT="{COMPILER}"\nCONFIG_LLD_VERSION=220103\n'
        for name, contents in {
            "Makefile": "# Fake kernel for host boundary tests\n",
            ".config": config,
            "include/config/auto.conf": config,
            "include/config/kernel.release": RELEASE + "\n",
            "include/generated/utsrelease.h": f'#define UTS_RELEASE "{RELEASE}"\n',
            "include/generated/autoconf.h": "".join(f"#define CONFIG_{name} 1\n" for name in OPTIONS),
            "Module.symvers": "0x00000000\texample\tvmlinux\tEXPORT_SYMBOL\n",
            "vmlinux": "fake AArch64 ELF\n",
        }.items():
            path = self.kernel / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(contents)
        self.tool("uname", 'case "$1" in -m) echo aarch64;; -s) echo Linux;; esac\n')
        self.tool("clang", f"echo '{COMPILER}'\n")
        self.tool("ld.lld", "echo 'LLD 22.1.3 (compatible with GNU linkers)'\n")
        self.tool("llvm-readelf", "echo '  Machine: AArch64'\n")
        self.tool("modinfo", f'''case "$1" in
--version) echo 'kmod version 34';;
-F) echo '{RELEASE} SMP preempt mod_unload aarch64';;
*) echo 'test module metadata';;
esac
''')
        self.tool("make", f'''for argument do
    case "$argument" in
        kernelrelease) echo '{RELEASE}'; exit 0;;
        M=*) module_work=${{argument#M=}};;
        clean) cleaning=1;;
    esac
done
test -n "$module_work"
test -z "${{MAKEFLAGS-}}${{KCFLAGS-}}${{KBUILD_MODPOST_WARN-}}${{CONFIG_CFI-}}${{KBUILD_EXTRA_SYMBOLS-}}"
test "$LC_ALL" = C
name=$(sed -n 's/^obj-m := \\(.*\\)\\.o$/\\1/p' "$module_work/Makefile")
if [ "${{cleaning-}}" = 1 ]; then
    rm -f "$module_work/$name.o" "$module_work/$name.ko"
    exit 0
fi
printf 'fake candidate module\\n' > "$module_work/$name.ko"
''')

    def tool(self, name, body):
        path = self.bin / name
        path.write_text("#!/bin/sh\nset -eu\n" + body)
        path.chmod(0o755)

    def run_build(self, expected_success=False, extra_env=None, jobs="2"):
        environment = dict(os.environ, PATH=str(self.bin) + os.pathsep + os.environ["PATH"])
        environment.update(extra_env or {})
        result = subprocess.run(
            ["sh", str(self.script), str(self.kernel), RELEASE, str(self.output), jobs],
            env=environment, text=True, capture_output=True,
        )
        self.assertEqual(result.returncode == 0, expected_success, result.stdout + result.stderr)
        return result

    def rejected_before_output(self):
        self.run_build()
        self.assertFalse(self.output.exists())

    def test_success_builds_all_five_copies_and_scrubs_make_overrides(self):
        result = self.run_build(True, {
            "MAKEFLAGS": "-i", "KCFLAGS": "-fno-sanitize=kcfi", "CONFIG_CFI": "n",
            "KBUILD_MODPOST_WARN": "1", "KBUILD_EXTRA_SYMBOLS": "/fake/symbols",
        })
        self.assertIn("experimental candidate", result.stdout)
        for directory, name in DIRECTORIES.items():
            self.assertTrue((self.output / directory / f"{name}.ko").is_file())
            self.assertFalse((self.test_kit / "modules" / directory / f"{name}.ko").exists())
        self.assertEqual(len((self.output / "module-sha256.txt").read_text().splitlines()), 5)
        self.assertIn("experimental-candidate-only", (self.output / "build-inputs.txt").read_text())

    def test_missing_complete_kernel_input(self):
        for name in ("Module.symvers", "vmlinux", "include/generated/autoconf.h", "include/config/auto.conf"):
            with self.subTest(name=name):
                path = self.kernel / name
                data = path.read_bytes()
                path.unlink()
                self.rejected_before_output()
                path.write_bytes(data)

    def test_kernel_release_mismatch(self):
        (self.kernel / "include/config/kernel.release").write_text("other\n")
        self.rejected_before_output()

    def test_generated_release_mismatch(self):
        (self.kernel / "include/generated/utsrelease.h").write_text('#define UTS_RELEASE "other"\n')
        self.rejected_before_output()

    def test_make_release_mismatch(self):
        self.tool("make", "echo other\n")
        self.rejected_before_output()

    def test_compiler_mismatch(self):
        self.tool("clang", "echo 'Alpine clang version 22.1.4'\n")
        self.rejected_before_output()

    def test_linker_mismatch(self):
        self.tool("ld.lld", "echo 'LLD 22.1.4'\n")
        self.rejected_before_output()

    def test_vendor_prefixed_linker_version(self):
        self.tool("ld.lld", "echo 'Alpine LLD 22.1.3 (compatible with GNU linkers)'\n")
        self.run_build(True)

    def test_zero_jobs_rejected_before_output(self):
        for jobs in ("0", "00", "000"):
            with self.subTest(jobs=jobs):
                self.run_build(jobs=jobs)
                self.assertFalse(self.output.exists())

    def test_new_output_parent_created(self):
        self.output = self.root / "new" / "work" / "modules"
        self.run_build(True)
        self.assertTrue((self.output / "module-sha256.txt").is_file())

    def test_only_copied_build_artifacts_are_cleaned(self):
        for directory, name in DIRECTORIES.items():
            source = self.test_kit / "modules" / directory
            (source / f"{name}.o").write_text("old object")
            (source / f"{name}.ko").write_text("old module")
        self.run_build(True)
        for directory, name in DIRECTORIES.items():
            source = self.test_kit / "modules" / directory
            copied = self.output / directory
            self.assertEqual((source / f"{name}.o").read_text(), "old object")
            self.assertEqual((source / f"{name}.ko").read_text(), "old module")
            self.assertFalse((copied / f"{name}.o").exists())
            self.assertEqual((copied / f"{name}.ko").read_text(), "fake candidate module\n")
            self.assertTrue((copied / "clean.log").is_file())

    def test_required_configuration_and_generated_configuration(self):
        for name in (".config", "include/config/auto.conf", "include/generated/autoconf.h"):
            with self.subTest(name=name):
                path = self.kernel / name
                data = path.read_text()
                path.write_text(data.replace("CONFIG_CFI", "CONFIG_UNUSED"))
                self.rejected_before_output()
                path.write_text(data)

    def test_permissive_cfi_rejected(self):
        with (self.kernel / ".config").open("a") as output:
            output.write("CONFIG_CFI_PERMISSIVE=y\n")
        self.rejected_before_output()

    def test_non_native_host_rejected(self):
        self.tool("uname", "echo x86_64\n")
        self.rejected_before_output()

    def test_existing_output_preserved(self):
        self.output.mkdir()
        sentinel = self.output / "sentinel"
        sentinel.write_text("preserve me")
        self.run_build()
        self.assertEqual(sentinel.read_text(), "preserve me")

    def test_dangling_output_symlink_preserved(self):
        self.output.symlink_to(self.root / "absent")
        self.run_build()
        self.assertTrue(self.output.is_symlink())
        self.assertFalse((self.root / "absent").exists())

    def test_failed_module_build_stops_with_log(self):
        self.tool("make", f'''case "$*" in *kernelrelease*) echo '{RELEASE}'; exit 0;; *clean*) exit 0;; esac
echo 'modpost: unknown symbol' >&2
exit 2
''')
        self.run_build()
        self.assertIn("unknown symbol", (self.output / next(iter(DIRECTORIES)) / "build.log").read_text())
        self.assertFalse((self.output / "module-sha256.txt").exists())

    def test_wrong_module_vermagic_rejected(self):
        self.tool("modinfo", 'case "$1" in --version) echo "kmod version 34";; *) echo "other SMP";; esac\n')
        self.run_build()
        self.assertFalse((self.output / "module-sha256.txt").exists())


if __name__ == "__main__":
    unittest.main()
