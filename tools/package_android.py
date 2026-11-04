#!/usr/bin/env python3
"""Packages the Forza Motorsport 2 Android build into a signed, installable APK.

Adapted from tools/pinyon_android.py of arcanite24/pinyon-shift (BSD 3-Clause,
Copyright (c) 2026 Pinyon Shift contributors). No game files are packaged.

Needs the Android SDK (build-tools, platform, NDK) and a JDK 17+, found through
ANDROID_HOME, NDK_VERSION, BUILD_TOOLS and PLATFORM (or the options below).
"""

from __future__ import annotations

import argparse
import os
import platform
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

ABI = "arm64-v8a"
MIN_SDK = 33
TARGET_SDK = 35
REQUIRED = ("librexruntime.so", "librexgpu-fh1.so", "libmain.so")
# libadrenotools' hooks: the custom GPU driver loader (Mesa Turnip) finds them
# in the native library folder. Packaged when the build produced them.
OPTIONAL = ("libhook_impl.so", "libmain_hook.so", "libfile_redirect_hook.so",
            "libgsl_alloc_hook.so")


def run(command, **kwargs):
    command = [str(part) for part in command]
    print("+", " ".join(command), flush=True)
    subprocess.run(command, check=True, **kwargs)


def newest(directory: Path, name: str) -> Path | None:
    found = [path for path in directory.rglob(name) if "CMakeFiles" not in path.parts]
    return max(found, key=lambda path: path.stat().st_mtime) if found else None


def main() -> int:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--sdk-dir", type=Path, required=True, help="the ReXGlue SDK source tree")
    parser.add_argument("--android-home", type=Path, default=os.environ.get("ANDROID_HOME"))
    parser.add_argument("--ndk", default=os.environ.get("NDK_VERSION"))
    parser.add_argument("--build-tools", default=os.environ.get("BUILD_TOOLS"))
    parser.add_argument("--platform", default=os.environ.get("PLATFORM"))
    parser.add_argument("--keystore", type=Path, required=True,
                        help="signing key; made on first use, keep it to update installs in place")
    parser.add_argument("--version-name", default="0.1.0")
    parser.add_argument("--version-code", type=int, default=1)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    home = Path(args.android_home)
    build_tools = home / "build-tools" / args.build_tools
    android_jar = home / "platforms" / args.platform / "android.jar"
    host = "windows-x86_64" if os.name == "nt" else f"{platform.system().lower()}-x86_64"
    llvm = home / "ndk" / args.ndk / "toolchains" / "llvm" / "prebuilt" / host
    libcxx = llvm / "sysroot" / "usr" / "lib" / "aarch64-linux-android" / "libc++_shared.so"
    aapt2 = build_tools / "aapt2"
    d8 = build_tools / "d8"
    zipalign = build_tools / "zipalign"
    apksigner = build_tools / "apksigner"
    for tool in (aapt2, d8, zipalign, apksigner, android_jar, libcxx, llvm / "bin" / "llvm-strip"):
        if not tool.exists():
            print(f"error: {tool} not found", file=sys.stderr)
            return 1

    libraries = []
    for name in REQUIRED:
        found = newest(args.build_dir, name)
        if not found:
            print(f"error: {name} is not built in {args.build_dir}", file=sys.stderr)
            return 1
        libraries.append(found)
    for name in OPTIONAL:
        found = newest(args.build_dir, name)
        if found:
            libraries.append(found)
        else:
            print(f"note: {name} not built; custom GPU drivers will not load")
    libraries.append(libcxx)

    work = args.output.parent / "package"
    if work.exists():
        shutil.rmtree(work)
    for sub in ("classes", "dex", "lib"):
        (work / sub).mkdir(parents=True)
    unsigned = work / "unsigned.apk"

    resources = work / "resources.zip"
    run([aapt2, "compile", "--dir", repo / "android" / "res", "-o", resources])
    run([aapt2, "link", "-o", unsigned, "-I", android_jar,
         "--manifest", repo / "android" / "AndroidManifest.xml", resources,
         "--min-sdk-version", MIN_SDK, "--target-sdk-version", TARGET_SDK,
         "--version-name", args.version_name, "--version-code", args.version_code])

    sdl_java = args.sdk_dir / "thirdparty" / "sdl3" / "android-project" / "app" / "src" / "main" / "java"
    sources = sorted(sdl_java.rglob("*.java")) + sorted((repo / "android" / "java").rglob("*.java"))
    run(["javac", "-source", "1.8", "-target", "1.8", "-nowarn", "-Xlint:-options",
         "-bootclasspath", android_jar, "-classpath", android_jar,
         "-encoding", "UTF-8", "-d", work / "classes", *sources])
    classes = work / "classes.jar"
    with zipfile.ZipFile(classes, "w", zipfile.ZIP_DEFLATED) as jar:
        for path in sorted((work / "classes").rglob("*.class")):
            jar.write(path, path.relative_to(work / "classes").as_posix())
    run([d8, "--release", "--min-api", MIN_SDK, "--lib", android_jar,
         "--output", work / "dex", classes])

    # The device gets libraries without debug sections.
    stripped = []
    for library in libraries:
        target = work / "lib" / library.name
        run([llvm / "bin" / "llvm-strip", "--strip-debug", "-o", target, library])
        stripped.append(target)

    # Devices with 16 KiB pages refuse libraries whose load segments are
    # aligned to less.
    for library in stripped:
        listing = subprocess.run([str(llvm / "bin" / "llvm-readelf"), "-lW", str(library)],
                                 capture_output=True, text=True).stdout.splitlines()
        aligns = {int(line.split()[-1], 16) for line in listing if line.strip().startswith("LOAD")}
        if not aligns or min(aligns) < 0x4000:
            print(f"error: {library.name} has load segments aligned below 16 KiB", file=sys.stderr)
            return 1

    with zipfile.ZipFile(unsigned, "a", compression=zipfile.ZIP_DEFLATED) as apk:
        apk.write(work / "dex" / "classes.dex", "classes.dex")
        for library in stripped:
            apk.write(library, f"lib/{ABI}/{library.name}")
        apk.write(repo / "config" / "forza2.toml", "assets/forza2.toml")
        apk.write(repo / "config" / "forza2.android.toml", "assets/forza2.android.toml")

    aligned = work / "aligned.apk"
    run([zipalign, "-P", "16", "-f", "4", unsigned, aligned])

    if not args.keystore.is_file():
        args.keystore.parent.mkdir(parents=True, exist_ok=True)
        run(["keytool", "-genkeypair", "-keystore", args.keystore, "-storepass", "forza2-local",
             "-keypass", "forza2-local", "-alias", "forza2", "-keyalg", "RSA", "-keysize", "3072",
             "-validity", "10000", "-dname", "CN=Forza2 Android local build"],
            stdout=subprocess.DEVNULL)
    run([apksigner, "sign", "--ks", args.keystore, "--ks-pass", "pass:forza2-local",
         "--key-pass", "pass:forza2-local", "--out", args.output, aligned])
    print(f"apk: {args.output} ({args.output.stat().st_size} bytes)")
    print("libraries: " + ", ".join(path.name for path in stripped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
