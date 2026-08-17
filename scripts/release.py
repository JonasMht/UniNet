#!/usr/bin/env python3
"""UniNet releases: one command, every target, nothing unverifiable.

The project builds for Python, C++, C# and 3D Slicer, and until now creating a
"version" meant remembering five different commands that each know about a
different file, none of them able to say whether the other four agree. The
version number is declared in five places, and a release where they disagree
is a release where the Slicer installer refuses a wheel that pip would serve:
"0.2.0" means different things to different consumers.

    python3 scripts/release.py                     # check everything (default)
    python3 scripts/release.py check
    python3 scripts/release.py bump 0.3.0          # one source of truth, five files
    python3 scripts/release.py package             # build every target on this machine
    python3 scripts/release.py package --targets python,cpp --skip-tests
    python3 scripts/release.py package --slicer /path/to/Slicer-5.10.0-linux-amd64

Every artifact lands in dist/release-<version>/ together with a MANIFEST that
records the exact git commit it was built from and a SHA256SUMS file, so an
artifact can always be traced back to the tree that produced it - including,
for wheels, inside the wheel itself (uninet.__build__).

Principles, matching the rest of the repository:

  * Nothing is released from an unmodified, packed tree that does not say so.
    package refuses to run on a dirty checkout unless told otherwise.
  * Nothing is released when the five version declarations disagree. bump
    changes all five at once; anyone editing one of them by hand is told that
    the other four do not match, with exactly which ones.
  * Nothing is released broken: the fast gate (ctest + pytest) runs first.
  * A machine that cannot build a target says so and names the missing tool;
    a SKIP is never a PASS, but a missing toolchain is not a failed release.

Works on Python 3.9+ with only the standard library plus the UniNetSlicer
helpers this repository already ships. The C# target additionally needs the
.NET SDK, the Slicer target needs a Slicer installation.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

# UniNetSlicer owns all the wheel-stamping machinery; importing it is
# deliberately zero-cost (no Slicer is touched at import time).
sys.path.insert(0, str(HERE))
import UniNetSlicer as us                                   # noqa: E402

VERSION_FILES = {
    "CMakeLists.txt": r'project\(UniNet\s+LANGUAGES C CXX VERSION\s+([0-9][^ )]*)',
    "pyproject.toml": r'^\s*version\s*=\s*"([^"]+)"',
    "python/uninet/__init__.py": r'__version__\s*=\s*"([^"]+)"',
    "scripts/UniNetSlicer.py": r'REQUIRED_VERSION\s*=\s*"([^"]+)"',
    "csharp/UniNet/UniNet.csproj": r'<Version>([^<]+)</Version>',
}


def say(msg: str) -> None:
    print(f"[release] {msg}", flush=True)


def fail(msg: str) -> None:
    print(f"[release] ! {msg}", flush=True)


def versions() -> dict[str, str]:
    """The version each file declares, or "" if its declaration is missing."""
    found: dict[str, str] = {}
    for rel, pattern in VERSION_FILES.items():
        path = ROOT / rel
        text = path.read_text(encoding="utf-8")
        match = re.search(pattern, text, re.M)
        found[rel] = match.group(1) if match else ""
    return found


def check_versions() -> list[str]:
    """Files whose declared version differs from pyproject.toml's."""
    found = versions()
    reference = found.get("pyproject.toml", "")
    return [rel for rel, value in found.items()
            if value and value != reference]


def repo_head() -> str:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                             capture_output=True, text=True, check=False)
        if out.returncode == 0:
            return out.stdout.strip()
    except OSError:
        pass
    return ""


def tree_is_clean() -> bool:
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "status", "--porcelain"],
                             capture_output=True, text=True, check=False)
        return out.returncode == 0 and not out.stdout.strip()
    except OSError:
        return False


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _run(command: list[str], label: str, env: dict | None = None) -> tuple[int, str]:
    """Run a subprocess, returning (exit code, tail of combined output)."""
    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env)
    except FileNotFoundError:
        return 127, f"command not found: {command[0]}"
    output = (result.stdout + result.stderr).strip()
    return result.returncode, output[-4000:]


def cmd_check(args: argparse.Namespace) -> int:
    found = versions()
    reference = found.get("pyproject.toml", "")
    say(f"reference version (pyproject.toml): {reference or '(missing!)'}")
    mismatched = check_versions()
    if mismatched:
        fail("these files disagree with pyproject.toml:")
        for rel in mismatched:
            fail(f"  {rel}: {found[rel] or '(no version declaration found)'}")
        say("fix with:  python3 scripts/release.py bump <version>")
        return 2

    if repo_head():
        say(f"tree HEAD: {repo_head()[:12]}")
    say("version declarations: consistent")
    say("targets:")
    for target, tool in (("python", "python3"), ("cpp", "cmake"),
                         ("csharp", "dotnet"), ("slicer", "slicer")):
        available = bool(shutil.which(tool)) or target == "python"
        say(f"  {target:8s} {'available' if available else 'MISSING'}")
    if not tree_is_clean():
        say("note: working tree has uncommitted changes (package will refuse "
            "unless --allow-dirty)")
    return 0


def cmd_bump(args: argparse.Namespace) -> int:
    new = args.version
    if not re.fullmatch(r"\d+\.\d+\.\d+", new):
        fail(f"{new!r} is not a PEP 440-style version (X.Y.Z)")
        return 2
    old = versions().get("pyproject.toml", "")
    if not old:
        fail("could not read the current version from pyproject.toml")
        return 2
    if old == new:
        say("already at that version; nothing to change")
        return 0

    def replace(path: Path, pattern: str, value: str) -> bool:
        text = path.read_text(encoding="utf-8")
        updated, count = re.subn(pattern, value, text, count=1)
        if count == 1:
            path.write_text(updated, encoding="utf-8")
        return count == 1

    changed: list[str] = []
    for rel, pattern in VERSION_FILES.items():
        # Rebuild the replacement with the captured value substituted.
        def repl(match: re.Match) -> str:
            return match.group(0)[:match.start(1) - match.start(0)] + new \
                   + match.group(0)[match.end(1) - match.start(0):]

        path = ROOT / rel
        text = path.read_text(encoding="utf-8")
        updated, count = re.subn(pattern, repl, text, count=1, flags=re.M)
        if count == 1:
            path.write_text(updated, encoding="utf-8")
            changed.append(rel)

    # README mentions the version as prose in a couple of places that must not
    # drift; a bounded replace of the exact old version token keeps them honest.
    readme = ROOT / "README.md"
    count = 0
    if old:
        text = readme.read_text(encoding="utf-8")
        text, count = re.subn(rf"\b{re.escape(old)}\b", new, text)
        if count:
            readme.write_text(text, encoding="utf-8")

    say(f"bumped {old} -> {new}")
    for rel in changed:
        say(f"  {rel}")
    say(f"  README.md ({count} mention(s))" if count else "  README.md (no version mentions)")
    say("next:  git add -A && git commit -m 'bump version to %s' && "
        "git tag v%s && git push origin main v%s" % (new, new, new))
    return 0


# ── building the targets ─────────────────────────────────────────────────────

def _builtin_build_dir() -> Path:
    return ROOT / "build-release"


def target_python(out_dir: Path, mkdir: bool = True) -> Path | None:
    """A stamped cp-ABI wheel for the running interpreter."""
    python = sys.executable
    stamp_file = ROOT / "python" / "uninet" / "_buildinfo.py"
    commit = us._source_commit(ROOT)
    wheels: list[Path] = []
    if mkdir:
        out_dir.mkdir(parents=True, exist_ok=True)
    try:
        if commit:
            us._stamp_buildinfo(ROOT, commit)
        env = dict(os.environ)
        env["SKBUILD_BUILD_DIR"] = str(ROOT / "_build-release-py")
        code, output = _run(
            [python, "-m", "pip", "wheel", "--no-deps", "--no-cache-dir",
             "--wheel-dir", str(out_dir), str(ROOT)],
            "python wheel")
        if code != 0:
            fail("pip wheel failed; tail of output:")
            print(output)
            return None
        wheels = sorted(out_dir.glob("uninet-*.whl"), key=lambda p: p.stat().st_mtime)
        if not wheels:
            fail("pip wheel reported success but produced no wheel")
            return None
        built = wheels[-1]
        if commit and not us._ensure_wheel_stamp(built, commit):
            fail(f"could not stamp {built.name}")
        say(f"python: {built.name} ({commit[:12]})") if commit else say(f"python: {built.name}")
        return built
    finally:
        try:
            stamp_file.unlink()
        except OSError:
            pass


def target_cpp(out_dir: Path) -> Path | None:
    """A self-contained C SDK bundle: headers, shared library, tools, in tar.gz."""
    build = _builtin_build_dir()
    cmake = shutil.which("cmake")
    if cmake is None:
        fail("cmake is not installed; cannot build the C++ package")
        return None
    say("building the self-contained C SDK (fetches and compiles ZeroMQ, czmq, "
        "zyre once: a few minutes)")
    code, output = _run(
        [cmake, "-S", str(ROOT), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
         "-DUNINET_BUILD_PYTHON=OFF", "-DUNINET_BUILD_CABI=ON",
         "-DUNINET_SELF_CONTAINED=ON"],
        "cmake configure")
    if code != 0:
        fail("cmake configure failed; tail of output:")
        print(output)
        return None
    code, output = _run([cmake, "--build", str(build),
                         "-j", str(os.cpu_count() or 4)], "cmake build")
    if code != 0:
        fail("cmake build failed; tail of output:")
        print(output)
        return None

    stage = out_dir / f"uninet-cpp-SDK-{versions()['pyproject.toml']}"
    code, output = _run([cmake, "--install", str(build), "--prefix", str(stage)],
                        "cmake install")
    if code != 0:
        fail("cmake install failed; tail of output:")
        print(output)
        return None

    machine = platform.machine().lower() or "unknown"
    archive = out_dir / (
        f"uninet-cpp-SDK-{versions()['pyproject.toml']}-"
        f"{platform.system().lower()}-{machine}.tar.gz")
    with tarfile.open(archive, "w:gz") as tar:
        tar.add(stage, arcname=stage.name)
    shutil.rmtree(stage, ignore_errors=True)
    say(f"cpp: {archive.name}")
    return archive


def target_csharp(out_dir: Path, native_dir: Path | None) -> Path | None:
    dotnet = shutil.which("dotnet")
    if dotnet is None:
        fail("dotnet is not installed; the C# NuGet package cannot be built "
             "(https://dotnet.microsoft.com/download)")
        return None
    if native_dir is None or not _has_native_lib(native_dir):
        fail(f"no native library at {native_dir}; build the C SDK first "
             "(package --targets cpp,csharp)")
        return None
    code, output = _run(
        [dotnet, "pack", str(ROOT / "csharp" / "UniNet" / "UniNet.csproj"),
         "-c", "Release", "-o", str(out_dir),
         f"-p:UniNetNativeDir={native_dir}"],
        "dotnet pack")
    if code != 0:
        fail("dotnet pack failed; tail of output:")
        print(output)
        return None
    packages = sorted(out_dir.glob("*.nupkg"), key=lambda p: p.stat().st_mtime)
    if not packages:
        fail("dotnet pack reported success but produced no .nupkg")
        return None
    say(f"csharp: {packages[-1].name}")
    return packages[-1]


def _has_native_lib(directory: Path) -> bool:
    """The C ABI shared library this platform's .NET would load."""
    return any((directory / name).is_file()
               for name in ("libuninet_c.so", "libuninet_c.dylib",
                            "uninet_c.dll", "Release/uninet_c.dll"))


def target_slicer(out_dir: Path, slicer: str | None, source: Path) -> Path | None:
    try:
        home = us.find_slicer(slicer)
    except us.SetupError as exc:
        fail(f"no Slicer found ({exc}); the Slicer wheel is skipped. "
             "Pass --slicer /path/to/Slicer-...-linux-amd64")
        return None
    try:
        wheel = us.make_wheel(home, source)
    except us.SetupError as exc:
        fail(f"building the Slicer wheel failed: {exc}")
        return None
    target = out_dir / wheel.name
    shutil.copy2(wheel, target)
    say(f"slicer: {target.name}")
    return target


def cmd_package(args: argparse.Namespace) -> int:
    mismatched = check_versions()
    if mismatched:
        fail("version declarations disagree; refusing to package:")
        for rel in mismatched:
            fail(f"  {rel}: {versions()[rel]}")
        say("fix with:  python3 scripts/release.py bump <version>")
        return 2
    if not args.allow_dirty and not tree_is_clean():
        fail("the working tree has uncommitted changes; a release must be made "
             "from the tree the commit describes. Commit first, or pass "
             "--allow-dirty.")
        return 2

    if not args.skip_tests:
        say("running the test gate (ctest + pytest)")
        code, output = _run(["cmake", "--build",
                             str(args.build_dir or "build"),
                             "-j", str(os.cpu_count() or 4)], "gate build")
        if code not in (0, 127):
            fail("test-gate build failed; tail of output:")
            print(output)
            return 2
        code, output = _run(
            ["ctest", "--test-dir", str(args.build_dir or "build"), "-L",
             "uninet", "--no-tests=error", "--output-on-failure"], "ctest")
        if code != 0:
            fail("ctest failed; tail of output:")
            print(output)
            return 2
        env = dict(os.environ, PYTHONPATH=str(ROOT / "python"))
        code, output = _run([sys.executable, "-m", "pytest",
                             str(ROOT / "python" / "tests"), "-q"],
                            "pytest", env=env)
        if code != 0:
            fail("pytest failed; tail of output:")
            print(output)
            return 2
        say("test gate: PASS")

    version = versions()["pyproject.toml"]
    out_dir = ROOT / "dist" / f"release-{version}"
    out_dir.mkdir(parents=True, exist_ok=True)

    targets = {t.strip() for t in args.targets.split(",") if t.strip()}
    if "all" in targets:
        targets = {"python", "cpp", "csharp", "slicer"}
    requested_explicitly = args.targets.strip().lower() not in ("all", "", "python,cpp,csharp,slicer")

    produced: dict[str, Path] = {}
    order = ("python", "cpp", "csharp", "slicer")
    for name in order:
        if name not in targets:
            continue
        if name == "python":
            artifact = target_python(out_dir)
        elif name == "cpp":
            artifact = target_cpp(out_dir)
        elif name == "csharp":
            artifact = target_csharp(out_dir, _builtin_build_dir())
        elif name == "slicer":
            artifact = target_slicer(out_dir, args.slicer, ROOT)
        else:
            artifact = None
        if artifact:
            produced[name] = artifact

    head = repo_head()
    manifest_lines = [
        f"uninet release {version}",
        f"commit:      {head or '(not a git checkout)'}",
        f"machine:     {platform.platform()}",
        f"python:      {platform.python_version()}",
        "",
    ]
    for name in order:
        artifact = produced.get(name)
        if artifact:
            manifest_lines.append(f"{name:8s} {artifact.name}  "
                                  f"sha256={_sha256(artifact)}  "
                                  f"size={artifact.stat().st_size}")
        elif name in targets and any(
                name == n for n in ("python", "cpp", "csharp", "slicer")):
            manifest_lines.append(f"{name:8s} (skipped on this machine)")
    manifest = out_dir / "MANIFEST.txt"
    manifest.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")

    sums = out_dir / "SHA256SUMS"
    lines = []
    for artifact in produced.values():
        lines.append(f"{_sha256(artifact)}  {artifact.name}")
    sums.write_text("\n".join(lines) + "\n", encoding="utf-8")

    say(f"artifacts: {out_dir}")
    for name in order:
        artifact = produced.get(name)
        say(f"  {name:8s} {'ok: ' + artifact.name if artifact else 'skipped'}")

    missing = [n for n in targets if n not in produced]
    if missing:
        say(f"targets not produced on this machine: {', '.join(missing)}")
        if requested_explicitly:
            fail("an explicitly requested target was not produced; see above")
            return 2
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="release.py", description="Build verifiable UniNet releases.")
    sub = parser.add_subparsers(dest="command")

    sub.add_parser("check", help="verify versions, tree and toolchains (default)")

    bump = sub.add_parser("bump", help="change the version in every declaration")
    bump.add_argument("version", help="new version, X.Y.Z")

    package = sub.add_parser(
        "package", help="build the requested targets into dist/release-<v>/")
    package.add_argument(
        "--targets", default="all",
        help="comma-separated: python,cpp,csharp,slicer (default: all)")
    package.add_argument("--slicer", default=None,
                         help="path to a Slicer installation (for the slicer target)")
    package.add_argument("--skip-tests", action="store_true",
                         help="do not run ctest+pytest before packaging")
    package.add_argument("--allow-dirty", action="store_true",
                         help="package even with uncommitted changes")
    package.add_argument("--build-dir", default=None,
                         help="existing build tree to run the test gate against")

    args = parser.parse_args(argv)
    if args.command in (None, "check"):
        return cmd_check(args)
    if args.command == "bump":
        return cmd_bump(args)
    if args.command == "package":
        return cmd_package(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
