#!/usr/bin/env python3
"""UniNet for 3D Slicer: install it once, then let Slicer keep itself right.

Slicer ships its own Python, so UniNet's compiled extension has to be built
against *that* interpreter: one built for the system Python will not load. Doing
that by hand means knowing where Slicer is, that its binary release ships no
Python headers, and which of cmake's several Python variables its pybind11
actually reads. This file knows all of it, so nobody else has to.

Four ways to use it, each of them one step:

  1. **In Slicer's Python console** (Ctrl+3 / View -> Python Console), with no
     terminal, no checkout and no build tools knowledge::

         exec(open("/path/to/UniNet/scripts/UniNetSlicer.py").read())

     It installs UniNet into this Slicer and offers to check again at every
     start, so a Slicer update cannot quietly leave you without it.

  2. **From a terminal**, on any Python 3.8+ (it drives Slicer's Python for
     you, it does not have to run under it)::

         python3 scripts/UniNetSlicer.py                 # find Slicer, install
         python3 scripts/UniNetSlicer.py status
         python3 scripts/UniNetSlicer.py hook            # check at every start
         python3 scripts/UniNetSlicer.py wheel           # file to hand around

  3. **From your own Slicer module**, so your users never see any of this::

         import UniNetSlicer
         uninet = UniNetSlicer.ensure()

  4. **With no checkout at all**, in Slicer's Python console::

         import urllib.request as u; exec(u.urlopen(
             "https://raw.githubusercontent.com/JonasMht/UniNet/main"
             "/scripts/UniNetSlicer.py").read().decode())

     which clones UniNet into a cache directory and installs from there.

Distribution: `wheel` produces a single .whl. Put it next to this file (or in
UNINET_WHEEL) and every other machine with the same Slicer version installs in
seconds, with no compiler and no network. Building is only the fallback.

The file is deliberately self-contained and dependency-free: it is meant to be
copied on its own.

Environment overrides, all optional:

  UNINET_SLICER   the Slicer directory to use (the one containing bin/)
  UNINET_SOURCE   the UniNet checkout to build from
  UNINET_WHEEL    a .whl file, or a directory to search for one
  UNINET_CACHE    where headers, wheels and build trees are kept
  UNINET_CC       the C compiler to build with (UNINET_CXX for C++)
  UNINET_GIT_URL  where to clone UniNet from, when there is no checkout
"""
from __future__ import annotations

import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

__all__ = [
    "ensure",
    "ensure_at_startup",
    "find_slicer",
    "install",
    "install_startup_hook",
    "make_wheel",
    "remove_startup_hook",
    "status",
    "uninstall",
]

#: The UniNet this file expects. Installations older than it are upgraded.
#: Bump together with pyproject.toml.
REQUIRED_VERSION = "0.2.0"

#: Where to clone from when no checkout is on the machine. UniNet may well live
#: somewhere else - an internal forge, a fork - and nothing else in this file
#: assumes GitHub, so this is a setting rather than a constant.
GIT_URL = os.environ.get("UNINET_GIT_URL") or "https://github.com/JonasMht/UniNet.git"
CPYTHON_SOURCE = "https://www.python.org/ftp/python/{v}/Python-{v}.tgz"

# The marker pair that delimits our block in .slicerrc.py. Everything between
# them is ours to rewrite; everything outside is the user's and is never touched.
HOOK_BEGIN = "# >>> UniNet (managed by UniNetSlicer.py) >>>"
HOOK_END = "# <<< UniNet <<<"


# ── talking to the user ──────────────────────────────────────────────────────
# Slicer's Python console is not a terminal, so no colour and no \r tricks:
# every line has to stand on its own in the log too.

def say(msg: str) -> None:
    print(f"[uninet] {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"[uninet] ! {msg}", flush=True)


class SetupError(RuntimeError):
    """Something the user has to fix. The message says what."""


# ── where things live ────────────────────────────────────────────────────────

def cache_dir() -> Path:
    """Headers, built wheels and build trees. Never inside the Slicer install:
    a Slicer update replaces that directory wholesale."""
    if os.environ.get("UNINET_CACHE"):
        return Path(os.environ["UNINET_CACHE"]).expanduser()
    if sys.platform == "win32":
        base = os.environ.get("LOCALAPPDATA") or (Path.home() / "AppData" / "Local")
        return Path(base) / "UniNet" / "cache"
    return Path.home() / ".cache" / "uninet"


def _script_dir() -> Path | None:
    """Where this file is, if it is a file at all. It has none when it was
    exec()'d from the Python console or fetched over HTTP, which are two of the
    four documented ways to run it."""
    try:
        return Path(__file__).resolve().parent
    except NameError:
        return None


def find_source() -> Path | None:
    """A UniNet checkout to build from, or None to clone one."""
    here = _script_dir()
    for candidate in (
        os.environ.get("UNINET_SOURCE"),
        # scripts/UniNetSlicer.py -> the repository root above it.
        (here.parent if here else None),
        cache_dir() / "src",
    ):
        if not candidate:
            continue
        root = Path(candidate).expanduser()
        if (root / "pyproject.toml").is_file() and (root / "CMakeLists.txt").is_file():
            return root
    return None


def source_version(source: Path) -> str | None:
    """The version the checkout would install, read without importing anything:
    tomllib is 3.11+ and this file must run on Slicer 5.6's 3.9 too."""
    try:
        text = (source / "pyproject.toml").read_text(encoding="utf-8")
    except OSError:
        return None
    match = re.search(r'^version\s*=\s*["\']([^"\']+)["\']', text, re.M)
    return match.group(1) if match else None


def clone_source() -> Path:
    dest = cache_dir() / "src"
    if (dest / "pyproject.toml").is_file():
        say(f"updating the UniNet checkout in {dest}")
        subprocess.run(["git", "-C", str(dest), "pull", "--ff-only"], check=False)
        return dest
    if not shutil.which("git"):
        raise SetupError(
            "No UniNet checkout was found and git is not installed, so one "
            "cannot be fetched. Either install git, or point UNINET_SOURCE at "
            "a checkout, or pass --source /path/to/UniNet."
        )
    say(f"cloning UniNet into {dest} (no checkout was found)")
    dest.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "clone", "--depth", "1", GIT_URL, str(dest)], check=True)
    return dest


# ── finding Slicer ───────────────────────────────────────────────────────────

def _python_slicer(home: Path) -> Path | None:
    """The interpreter launcher inside a Slicer installation. It is a launcher,
    not python itself: Slicer's `python-real` cannot start without PYTHONHOME
    and LD_LIBRARY_PATH pointing into the tree, and the launcher sets both."""
    for rel in (
        "bin/PythonSlicer",
        "bin/PythonSlicer.exe",
        "Contents/bin/PythonSlicer",       # macOS, given the .app itself
        "Contents/MacOS/PythonSlicer",
    ):
        candidate = home / rel
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    return None


def _slicer_candidates() -> list[Path]:
    """Every plausible Slicer installation on this machine, newest first.

    Slicer has no registry entry, no package manager entry and no fixed prefix:
    people unpack the release wherever they like. So this looks in the places
    the installers and the download page actually put it."""
    patterns: list[str] = []
    home = str(Path.home())
    if sys.platform == "win32":
        patterns += [
            rf"{home}\AppData\Local\NA-MIC\Slicer *",
            r"C:\Program Files\Slicer *",
            r"C:\ProgramData\NA-MIC\Slicer *",
        ]
    elif sys.platform == "darwin":
        patterns += [
            "/Applications/Slicer.app",
            "/Applications/Slicer*.app",
            f"{home}/Applications/Slicer*.app",
            f"{home}/Slicer*.app",
        ]
    else:
        patterns += [
            f"{home}/Slicer*",
            f"{home}/Documents/Slicer*",
            f"{home}/Downloads/Slicer*",
            f"{home}/opt/Slicer*",
            "/opt/Slicer*",
            "/usr/local/Slicer*",
            "/opt/slicer*",
        ]
    found: list[Path] = []
    for pattern in patterns:
        for path in glob.glob(pattern):
            root = Path(path)
            if _python_slicer(root):
                found.append(root)

    # An installation that is on PATH but in none of those places: follow the
    # launcher back to its own directory. `Slicer` is very often a symlink.
    launcher = shutil.which("Slicer") or shutil.which("Slicer.exe")
    if launcher:
        root = Path(os.path.realpath(launcher)).parent
        for candidate in (root, root.parent):
            if _python_slicer(candidate) and candidate not in found:
                found.append(candidate)

    def version_key(path: Path) -> tuple:
        numbers = re.findall(r"(\d+)\.(\d+)\.?(\d*)", path.name)
        if not numbers:
            return (0, 0, 0)
        major, minor, patch = numbers[-1]
        return (int(major), int(minor), int(patch or 0))

    unique = list(dict.fromkeys(found))
    unique.sort(key=version_key, reverse=True)
    return unique


def find_slicer(hint: str | None = None) -> Path:
    """The Slicer installation to work on.

    Inside Slicer this is free: the running application knows where it is. From
    a terminal it is a search, and an ambiguous result is reported rather than
    guessed at, because installing into the wrong Slicer looks exactly like
    installing into no Slicer at all."""
    for value in (hint, os.environ.get("UNINET_SLICER")):
        if value:
            root = Path(value).expanduser().resolve()
            if _python_slicer(root):
                return root
            raise SetupError(
                f"{root} does not look like a Slicer installation: no "
                f"bin/PythonSlicer inside it. Give the directory that contains "
                f"bin/, for example ~/Documents/Slicer-5.8.1-linux-amd64."
            )

    slicer_module = sys.modules.get("slicer")
    if slicer_module is not None and hasattr(slicer_module, "app"):
        return Path(slicer_module.app.slicerHome)

    # Running under Slicer's own interpreter (PythonSlicer script.py): the tree
    # is above sys.prefix, which points at lib/Python.
    prefix = Path(sys.prefix).resolve()
    for parent in (prefix, *prefix.parents):
        if _python_slicer(parent):
            return parent

    candidates = _slicer_candidates()
    if not candidates:
        raise SetupError(
            "No 3D Slicer installation was found. Pass its location:\n"
            "    python3 UniNetSlicer.py --slicer /path/to/Slicer-5.8.1-linux-amd64\n"
            "(the directory that contains bin/PythonSlicer), or set UNINET_SLICER."
        )
    if len(candidates) > 1:
        listing = "\n".join(f"    {c}" for c in candidates)
        say("several Slicer installations were found; using the first:")
        say(listing)
        say("pass --slicer to choose another.")
    return candidates[0]


# ── asking Slicer's Python about itself ──────────────────────────────────────

_PROBE = r"""
import json, sys, sysconfig
print(json.dumps({
    "version": "%d.%d.%d" % sys.version_info[:3],
    "xy": "%d.%d" % sys.version_info[:2],
    "site": sysconfig.get_paths()["purelib"],
    "include": sysconfig.get_paths()["include"],
    "prefix": sys.prefix,
    "ext_suffix": sysconfig.get_config_var("EXT_SUFFIX") or "",
    "platform": sysconfig.get_platform(),
}))
"""


def probe(pyslicer: Path) -> dict:
    """What Slicer's Python is, asked of Slicer's Python. Never inferred from
    the directory name: the two disagree across releases."""
    result = subprocess.run(
        [str(pyslicer), "-c", _PROBE],
        capture_output=True, text=True, env=child_env(),
    )
    if result.returncode != 0:
        raise SetupError(
            f"{pyslicer} could not be run:\n{result.stderr.strip()}"
        )
    return json.loads(result.stdout.strip().splitlines()[-1])


def child_env(extra: dict | None = None) -> dict:
    """The environment for anything we spawn.

    Two things have to go, and both cause failures that name something else:

    * PYTHONHOME / PYTHONPATH belonging to *our* interpreter would be inherited
      by Slicer's, which then imports half of one installation and half of the
      other.
    * LD_LIBRARY_PATH pointing into the Slicer tree (set by the launcher when
      this file runs under PythonSlicer, or by Slicer itself when it runs
      inside the application) is also seen by the compiler and by cmake's link
      checks, which then bind against Slicer's bundled libraries instead of the
      system ones.

    Inside Slicer there is an exact answer for this - the environment Slicer was
    started from, before it modified it - so use that when it is available."""
    slicer_module = sys.modules.get("slicer")
    env = None
    if slicer_module is not None and hasattr(slicer_module, "util"):
        try:
            env = dict(slicer_module.util.startupEnvironment())
        except Exception:                                   # noqa: BLE001
            env = None
    if env is None:
        env = dict(os.environ)
    for name in (
        "PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP", "PYTHONEXECUTABLE",
        "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH",
    ):
        env.pop(name, None)
    if extra:
        env.update({k: v for k, v in extra.items() if v is not None})
    return env


def installed(pyslicer: Path, load: bool = False) -> tuple[str | None, str | None]:
    """The UniNet already visible to Slicer: its version and where it is.

    Checked in a subprocess rather than by importing here, because "here" may
    be a completely different interpreter, and because importing uninet loads a
    shared library we may be about to replace.

    `load` decides how hard the check is. Finding the module is enough to answer
    "is something installed"; it is NOT enough to answer "does it work", because
    an extension built against the wrong Python, or missing a library it needs,
    is found and then fails on import. After installing, ask the harder
    question - otherwise a broken install reports success and the startup hook
    stays quiet about it for ever."""
    code = (
        "import importlib.util, sys\n"
        "for extra in sys.argv[1:]:\n"
        "    sys.path.insert(0, extra)\n"
        "spec = importlib.util.find_spec('uninet')\n"
        "if spec is None:\n"
        "    raise SystemExit\n"
        + ("import uninet\n" if load else "")
        +
        "try:\n"
        "    import importlib.metadata as m; version = m.version('uninet')\n"
        "except Exception:\n"
        # A directory copied into site-packages by hand (which is what the old
        # build-for-slicer.sh did) has no metadata, so read the module instead.
        "    import re, pathlib\n"
        "    text = pathlib.Path(spec.origin).read_text(encoding='utf-8')\n"
        "    found = re.search(r'__version__\\s*=\\s*[\"\\']([^\"\\']+)', text)\n"
        "    version = found.group(1) if found else '0.0.0'\n"
        "print(version)\n"
        "print(spec.origin)\n"
    )
    result = subprocess.run(
        [str(pyslicer), "-c", code, str(fallback_site(pyslicer))],
        capture_output=True, text=True, env=child_env(),
    )
    lines = result.stdout.strip().splitlines()
    if result.returncode != 0 or not lines:
        if load and result.returncode != 0 and result.stderr.strip():
            # The import failed rather than the module being absent. That is a
            # different problem and the message says which.
            raise SetupError(
                "UniNet is installed in this Slicer but cannot be imported:\n"
                + result.stderr.strip().splitlines()[-1]
            )
        return None, None
    return lines[0], (lines[1] if len(lines) > 1 else None)


def installed_version(pyslicer: Path, load: bool = False) -> str | None:
    return installed(pyslicer, load)[0]


def installed_build(pyslicer: Path) -> str | None:
    """The source commit the installed module was built from, when it carries
    one. Empty for a module with no stamp, "" when nothing is installed. The
    commit - not the version, which never changes - is what answers "is the
    installed UniNet the current one?"."""
    code = (
        "import importlib.util, sys\n"
        "for extra in sys.argv[1:]:\n"
        "    sys.path.insert(0, extra)\n"
        "import uninet\n"
        "print(getattr(uninet, '__build__', '') or '')\n"
    )
    result = subprocess.run(
        [str(pyslicer), "-c", code, str(fallback_site(pyslicer))],
        capture_output=True, text=True, env=child_env(),
    )
    if result.returncode != 0:
        return None
    return (result.stdout.strip() or "") or None


def _version_tuple(version: str) -> tuple:
    return tuple(int(part) for part in re.findall(r"\d+", version)[:3])


# ── the two traps in a Slicer binary release ─────────────────────────────────

def ensure_headers(info: dict) -> Path | None:
    """Slicer's Python headers, fetched if the release did not ship them.

    A Slicer binary release contains lib/Python/include/python3.x/pyconfig.h and
    nothing else, so any build of any C extension fails on "Python.h: No such
    file or directory". Slicer's Python is stock CPython, so the headers from
    python.org for the exact same version match it exactly; combined with
    Slicer's own pyconfig.h (which records how *its* Python was configured) the
    result is what the release should have shipped.

    Returns the directory to add to the include path, or None when Slicer did
    ship usable headers."""
    include = Path(info["include"])
    if (include / "Python.h").is_file():
        return None

    shim = cache_dir() / f"pyheaders-{info['version']}"
    if (shim / "Python.h").is_file():
        return shim

    say(f"Slicer ships no Python headers; fetching CPython {info['version']} to match")
    url = CPYTHON_SOURCE.format(v=info["version"])
    staging = Path(tempfile.mkdtemp(prefix="uninet-headers-"))
    try:
        archive = staging / "python.tgz"
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                archive.write_bytes(response.read())
        except Exception as exc:                            # noqa: BLE001
            raise SetupError(
                f"could not download {url}: {exc}\n"
                f"With no network, copy the Include/ directory of CPython "
                f"{info['version']} to {shim} by hand and re-run."
            ) from exc

        prefix = f"Python-{info['version']}/Include/"
        with tarfile.open(archive) as tar:
            members = []
            for member in tar.getmembers():
                if not member.name.startswith(prefix) or not member.isreg():
                    continue
                # Paths in the archive are trusted only after checking: a member
                # called ../../x would otherwise be extracted outside staging.
                if os.path.isabs(member.name) or ".." in Path(member.name).parts:
                    continue
                members.append(member)
            if not members:
                raise SetupError(f"{url} did not contain {prefix}")
            tar.extractall(staging, members=members)

        shim.parent.mkdir(parents=True, exist_ok=True)
        if shim.exists():
            shutil.rmtree(shim)
        shutil.copytree(staging / f"Python-{info['version']}" / "Include", shim)

        # Slicer's own pyconfig.h wins: it is the one that matches the binary.
        # On Windows CPython keeps its pyconfig.h in PC/, not Include/.
        slicer_config = include / "pyconfig.h"
        if slicer_config.is_file():
            shutil.copy2(slicer_config, shim / "pyconfig.h")
        elif not (shim / "pyconfig.h").is_file():
            raise SetupError(
                f"neither {include} nor CPython's Include/ has a pyconfig.h; "
                f"this Slicer release cannot be built against."
            )
        return shim
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def find_compiler() -> dict:
    """A C and C++ compiler for the build, as environment variables.

    Slicer's Python remembers the compiler it was *built* with - on the Linux
    releases that is /opt/rh/devtoolset-7/root/usr/bin/gcc, a path that exists
    on no user's machine - and pip exports it as CC. cmake then stops with
    "Could not find compiler set in environment variable CC", naming a compiler
    nobody asked for. Setting CC and CXX explicitly is the fix.

    On Windows cmake finds MSVC through the registry, and setting CC would send
    it looking for a Unix compiler instead, so the variables stay unset there
    and we only check that a compiler exists at all."""
    if sys.platform == "win32":
        has_msvc = bool(shutil.which("cl.exe"))
        if not has_msvc:
            vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
            vswhere = vswhere / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            if vswhere.is_file():
                result = subprocess.run(
                    [str(vswhere), "-latest", "-products", "*", "-requires",
                     "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                     "-property", "installationPath"],
                    capture_output=True, text=True,
                )
                has_msvc = bool(result.stdout.strip())
        if not has_msvc:
            raise SetupError(
                "No C++ compiler was found. Install the free \"Build Tools for "
                "Visual Studio\" with the \"Desktop development with C++\" "
                "workload, then re-run this. (Or drop a prebuilt uninet .whl "
                "next to this file - then no compiler is needed at all.)"
            )
        return {}

    c_compiler = os.environ.get("UNINET_CC") or _first_of("cc", "gcc", "clang")
    cxx_compiler = os.environ.get("UNINET_CXX") or _first_of("c++", "g++", "clang++")
    if not c_compiler or not cxx_compiler:
        hint = {
            "darwin": "Install Apple's command line tools:  xcode-select --install",
        }.get(sys.platform, "Install a C++ compiler, e.g.  sudo apt install build-essential")
        raise SetupError(
            f"No C++ compiler was found. {hint}\n"
            f"(Or drop a prebuilt uninet .whl next to this file - then no "
            f"compiler is needed at all.)"
        )
    return {"CC": c_compiler, "CXX": cxx_compiler}


def _first_of(*names: str) -> str | None:
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


# ── installing ───────────────────────────────────────────────────────────────

def fallback_site(pyslicer: Path) -> Path:
    """Where UniNet goes when Slicer's site-packages cannot be written to,
    which is the normal case for a system-wide install under /opt or Program
    Files. Slicer disables the per-user site directory, so this one is only on
    sys.path because the startup hook puts it there.

    Named after the installation, not the launcher: on macOS the launcher's
    parent directories are Contents/bin for every Slicer there is, so two
    installations would otherwise share one directory."""
    home = next((p for p in Path(pyslicer).parents if _python_slicer(p)),
                Path(pyslicer).parent.parent)
    return cache_dir() / f"site-{home.name}"


def _site_is_writable(info: dict) -> bool:
    site = Path(info["site"])
    probe_dir = site if site.is_dir() else site.parent
    return os.access(probe_dir, os.W_OK)


def find_wheel(info: dict, source: Path | None = None, on_skip=None) -> Path | None:
    """A prebuilt wheel for this exact interpreter, if somebody already built
    one. This is what makes the whole thing distributable: one colleague builds,
    everybody else installs a file in five seconds with no compiler.

    A wheel is only reused when it cannot be stale. A wheel is stale when it was
    built from an older source than the checkout we have - the version number
    stays 0.2.0 across fixes, so a build's age is visible only through the git
    commit stamped into it at build time. With a git source in hand, a cached
    wheel that does not match that commit is skipped (and said so), so "install"
    can no longer quietly keep applying last year's wheel."""
    desired = _source_commit(source) if source else ""
    tag = "cp" + info["xy"].replace(".", "")
    places: list[Path] = []
    override = os.environ.get("UNINET_WHEEL")
    if override:
        path = Path(override).expanduser()
        if path.is_file():
            return path
        places.append(path)
    here = _script_dir()
    if here:
        places += [here, here.parent / "dist", here.parent / "wheelhouse"]
    source_dir = find_source()
    if source_dir:
        places += [source_dir / "dist", source_dir / "wheelhouse"]
    places.append(cache_dir() / "wheels")

    for place in places:
        if not place.is_dir():
            continue
        for wheel in sorted(place.glob("uninet-*.whl"), reverse=True):
            name = wheel.name
            if tag not in name and "py3-none-any" not in name:
                continue
            # The platform tag is left to pip: it knows the whole compatibility
            # set (manylinux, macosx deployment targets), and a wheel it refuses
            # simply falls through to a source build below.
            if desired and _wheel_commit(wheel) not in ("", desired):
                if on_skip:
                    on_skip(wheel, desired)
                continue
            return wheel
    return None


def _run_streaming(command: list[str], env: dict, on_line=None) -> int:
    """Run a build, showing it as it happens. Slicer's Python console prints
    nothing until a subprocess ends, and a five-minute silence looks exactly
    like a hang, so every line is forwarded as it arrives."""
    process = subprocess.Popen(
        command, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    assert process.stdout is not None
    for line in process.stdout:
        line = line.rstrip()
        if on_line:
            on_line(line)
        else:
            print(f"    {line}", flush=True)
    return process.wait()


def _build_env(pyslicer: Path, info: dict) -> dict:
    """Everything the build needs to target Slicer's Python instead of ours."""
    shim = ensure_headers(info)
    extra = dict(find_compiler())
    # A build tree per Python version, kept out of the source checkout so the
    # source may be read-only (a network share, another user's home), and reused
    # so a second install does not fetch and compile zyre again.
    extra["SKBUILD_BUILD_DIR"] = str(cache_dir() / f"build-{info['xy']}")

    # CMake 4 removed compatibility with projects that ask for CMake < 3.5, and
    # libzmq, czmq and zyre all still do. scikit-build-core downloads a cmake
    # when the machine has none, and what it downloads is the newest one, so a
    # machine with no cmake installed was the one that failed. Pin it.
    extra["SKBUILD_CMAKE_VERSION"] = ">=3.15,<4"

    # Build every dependency from source even when the machine has it
    # installed. It costs a few minutes once, and it is what makes the wheel
    # worth passing around: linked against a system libzyre it loads only on
    # machines that also have that library, and the failure lands on the
    # colleague, at import time, reading "libzyre.so.2: cannot open shared
    # object file" about a library they never heard of. The same goes for the
    # optional libraries czmq links when it finds them - libsystemd and friends.
    cmake_args = ["-DUNINET_SELF_CONTAINED=ON"]
    if shim:
        # Both spellings on purpose: scikit-build-core passes Python_INCLUDE_DIR
        # from sysconfig (which points at the directory with only pyconfig.h in
        # it), pybind11 reads Python3_INCLUDE_DIR, and the legacy
        # FindPythonLibsNew path some pybind11 versions take reads neither and
        # only honours the include flags. Setting all three is what makes this
        # work across the pybind11 versions different Slicer releases ship.
        cmake_args += [f"-DPython_INCLUDE_DIR={shim}", f"-DPython3_INCLUDE_DIR={shim}"]
        for flag_var in ("CFLAGS", "CXXFLAGS"):
            existing = os.environ.get(flag_var, "")
            extra[flag_var] = f"{existing} -I{shim}".strip()
    # Appended to whatever the caller set, never replacing it: CMAKE_ARGS is how
    # someone passes their own option to this build, and it is the only way in.
    existing = os.environ.get("CMAKE_ARGS")
    extra["CMAKE_ARGS"] = " ".join(([existing] if existing else []) + cmake_args)
    return child_env(extra)


def _source_commit(source: Path) -> str:
    """The commit SHA the build is about to be made from, or "" if the source
    is not a git checkout. Best effort: no commit means no stamping and no
    staleness check, which is how this tool has always behaved."""
    try:
        out = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"],
            capture_output=True, text=True, check=False,
        )
    except OSError:
        return ""
    return out.stdout.strip() if out.returncode == 0 else ""


def _stamp_buildinfo(source: Path, commit: str) -> None:
    """Write python/uninet/_buildinfo.py so the wheel says which source it was
    built from. Removed again if the build fails: a source tree that does not
    carry _buildinfo.py must NOT have one left behind by this script."""
    target = source / "python" / "uninet" / "_buildinfo.py"
    try:
        target.write_text(
            f'"""Generated by UniNetSlicer.py; do not edit."""\n'
            f'BUILD_GIT = {commit!r}\n',
            encoding="utf-8",
        )
    except OSError:
        pass


def _wheel_commit(wheel: Path) -> str:
    """The BUILD_GIT stamp inside a wheel, or "" if it has none."""
    try:
        import zipfile
        with zipfile.ZipFile(wheel) as zf:
            text = zf.read("uninet/_buildinfo.py").decode("utf-8", "replace")
        match = re.search(r'BUILD_GIT\s*=\s*["\']([^"\']+)', text)
        return match.group(1) if match else ""
    except Exception:  # noqa: BLE001 - unreadable wheel => treat as unstamped
        return ""


def make_wheel(slicer: Path | str | None = None, source: Path | str | None = None,
               on_line=None) -> Path:
    """Build a redistributable wheel for this Slicer's Python and return it.

    The wheel lands in the cache and is picked up automatically by any later
    install on this machine; copy it next to this file to make installs on
    other machines instant."""
    home = find_slicer(str(slicer) if slicer else None)
    pyslicer = _python_slicer(home)
    if pyslicer is None:
        raise SetupError(f"{home} has no bin/PythonSlicer")
    info = probe(pyslicer)
    source = Path(source) if source else (find_source() or clone_source())

    # Stamp the source commit into the package so an installed module reports
    # exactly what it was built from. Without this, "uninet 0.2.0" that was
    # built last year and "uninet 0.2.0" built today are indistinguishable,
    # and a stale install cannot be told from a current one.
    commit = _source_commit(source)
    stamp_file = source / "python" / "uninet" / "_buildinfo.py"
    had_stamp = stamp_file.exists()
    if commit:
        _stamp_buildinfo(source, commit)

    wheel_dir = cache_dir() / "wheels"
    wheel_dir.mkdir(parents=True, exist_ok=True)
    tag = "cp" + info["xy"].replace(".", "")
    before = set(wheel_dir.glob(f"uninet-*{tag}*.whl"))

    say(f"building UniNet for Slicer's Python {info['version']} from {source}")
    say("the first build fetches and compiles ZeroMQ, czmq and zyre: a few minutes")
    env = _build_env(pyslicer, info)
    code = _run_streaming(
        [str(pyslicer), "-m", "pip", "wheel", "--no-deps", "--no-cache-dir",
         "--wheel-dir", str(wheel_dir), str(source)],
        env, on_line,
    )
    if not had_stamp:
        try:
            stamp_file.unlink()
        except OSError:
            pass
    if code != 0:
        raise SetupError(
            "the build failed; the output above says why. The usual causes are "
            "a missing compiler and no network access to fetch ZeroMQ."
        )
    wheels = sorted(wheel_dir.glob(f"uninet-*{tag}*.whl"), key=lambda p: p.stat().st_mtime)
    if not wheels:
        raise SetupError("the build reported success but produced no wheel")
    built = wheels[-1]
    # Older wheels for the same Python go only now, once there is something to
    # replace them with: cleaning up first would mean a failed build had thrown
    # away the working wheel that was there.
    for stale in before - {built}:
        stale.unlink()
    say(f"built {built.name}"
        + (f" ({commit[:12]})" if commit else ""))
    for line in _portability_warnings(built):
        warn(line)
    return built


def _portability_warnings(wheel: Path) -> list[str]:
    """Libraries this wheel would need on somebody else's machine, from places
    that machine probably does not have.

    ZeroMQ and friends are compiled in, but the build still links whatever else
    it found, and a library under /usr/local or in a home directory is one the
    build machine has and the next machine does not. The failure is silent until
    import, on the other person's computer, so it is worth a line here."""
    if sys.platform != "linux" or not shutil.which("ldd"):
        return []
    import zipfile                                          # noqa: PLC0415
    staging = Path(tempfile.mkdtemp(prefix="uninet-ldd-"))
    try:
        with zipfile.ZipFile(wheel) as archive:
            shared = [n for n in archive.namelist() if n.endswith(".so")]
            if not shared:
                return []
            extracted = archive.extract(shared[0], staging)
        listing = subprocess.run(["ldd", extracted], capture_output=True,
                                 text=True).stdout
    except Exception:                                       # noqa: BLE001
        return []
    finally:
        shutil.rmtree(staging, ignore_errors=True)

    unusual = []
    for line in listing.splitlines():
        if "=>" not in line:
            continue
        name, _, resolved = line.partition("=>")
        resolved = resolved.strip().split(" ")[0]
        if resolved.startswith(("/usr/local/", "/opt/", str(Path.home()))):
            unusual.append(f"{name.strip()} ({resolved})")
    if not unusual:
        return []
    return [
        "this wheel needs libraries from outside the system directories: "
        + ", ".join(unusual),
        "it will work here, but another machine without them cannot import it. "
        "Build the wheel somewhere plain (or in a container) to pass it around.",
    ]


def _remove_legacy_copy(info: dict) -> None:
    """An installation made by copying files into site-packages - which is what
    the old build-for-slicer.sh did - has no metadata, so pip cannot see it, and
    cannot remove it either. Left in place it shadows the new one and every
    change appears to do nothing."""
    site = Path(info["site"])
    package = site / "uninet"
    if not package.is_dir():
        return
    if list(site.glob("uninet-*.dist-info")) or list(site.glob("uninet-*.egg-info")):
        return
    say(f"removing an older hand-copied install at {package}")
    shutil.rmtree(package, ignore_errors=True)


def install(slicer: Path | str | None = None, source: Path | str | None = None,
            force: bool = False, build: bool = True, on_line=None) -> dict:
    """Make `import uninet` work in this Slicer, and report what happened.

    Order of preference: already installed, then a prebuilt wheel, then a build
    from source. Nothing is compiled that does not have to be."""
    # find_slicer even when given a path: it is what turns "that is not a Slicer"
    # into a message that says what was looked for and how to say it properly.
    home = find_slicer(str(slicer) if slicer else None)
    pyslicer = _python_slicer(home)
    if pyslicer is None:
        raise SetupError(f"{home} has no bin/PythonSlicer")
    info = probe(pyslicer)
    result = {"slicer": str(home), "python": info["version"], "action": "none"}

    have = installed_version(pyslicer)
    if have and not force and _version_tuple(have) >= _version_tuple(REQUIRED_VERSION):
        result.update(version=have, action="already installed")
        # By design install() does not touch an already-installed UniNet of a
        # sufficient version: the version never changes across fixes, so "it is
        # installed" says nothing about whether it is the CURRENT build. Report
        # the build stamp so this cannot be mistaken for an update having run.
        result["build"] = installed_build(pyslicer)
        return result

    _remove_legacy_copy(info)

    # An explicit --source means "build this", so the wheel search is skipped:
    # otherwise a cached wheel from an earlier build wins over the checkout the
    # caller just pointed at, and their change appears to have done nothing.
    # When a git checkout is available it is also what the staleness check in
    # find_wheel compares any cached wheel against.
    local = find_source() if source is None else (Path(source) if source else None)
    wheel = None if source else find_wheel(info, local, on_skip=lambda w, want: say(
        f"skipping {Path(w).name}: built from an older source than the current "
        f"checkout (has …{_wheel_commit(w)[:12] or 'no stamp'}, want …{want[:12]})"))
    if wheel:
        say(f"installing {wheel.name}")
        if _install_wheel(pyslicer, info, wheel, on_line) == 0:
            result.update(action="installed from wheel", wheel=str(wheel))
            result["version"] = installed_version(pyslicer, load=True)
            result["build"] = installed_build(pyslicer)
            return result
        warn("that wheel does not fit this Slicer's Python; building from source")

    if not build:
        raise SetupError("no usable wheel was found and building was not allowed")

    wheel = make_wheel(home, source, on_line)
    if _install_wheel(pyslicer, info, wheel, on_line) != 0:
        raise SetupError("the freshly built wheel could not be installed")
    result.update(action="built and installed", wheel=str(wheel))
    result["version"] = installed_version(pyslicer, load=True)
    return result


def _install_wheel(pyslicer: Path, info: dict, wheel: Path, on_line=None) -> int:
    command = [str(pyslicer), "-m", "pip", "install", "--no-deps",
               "--force-reinstall", "--no-warn-script-location"]
    if not _site_is_writable(info):
        target = fallback_site(pyslicer)
        target.mkdir(parents=True, exist_ok=True)
        warn(f"{info['site']} is not writable, installing into {target} instead")
        warn("the startup hook is what puts that directory on Slicer's path, so "
             "install it too (UniNetSlicer.py hook)")
        # --upgrade because pip skips an existing directory under --target
        # without it, which silently keeps the old build.
        command += ["--target", str(target), "--upgrade"]
    command.append(str(wheel))
    return _run_streaming(command, child_env(), on_line)


def uninstall(slicer: Path | str | None = None) -> None:
    home = find_slicer(str(slicer) if slicer else None)
    pyslicer = _python_slicer(home)
    if pyslicer is None:
        raise SetupError(f"{home} has no bin/PythonSlicer")
    info = probe(pyslicer)
    _run_streaming([str(pyslicer), "-m", "pip", "uninstall", "-y", "uninet"],
                   child_env())
    _remove_legacy_copy(info)
    target = fallback_site(pyslicer)
    if (target / "uninet").is_dir():
        shutil.rmtree(target / "uninet", ignore_errors=True)
        for meta in target.glob("uninet-*"):
            if meta.is_dir():
                shutil.rmtree(meta, ignore_errors=True)
            else:
                meta.unlink()
    say("UniNet removed from this Slicer")


# ── the startup hook ─────────────────────────────────────────────────────────

def slicerrc_path(home: Path) -> Path:
    """The file Slicer will actually execute at startup.

    Slicer looks for .slicerrc.py in its own installation directory first, then
    at $SLICERRC, then in the home directory. Writing to the home one while an
    installation-local one exists produces a hook that never runs, so the same
    order is followed here."""
    local = home / ".slicerrc.py"
    if local.is_file():
        return local
    if os.environ.get("SLICERRC"):
        return Path(os.environ["SLICERRC"]).expanduser()
    return Path.home() / ".slicerrc.py"


def _hook_block(script: Path, fallback: Path) -> str:
    return f"""{HOOK_BEGIN}
# Checks that UniNet is installed for this Slicer, and installs it if it is not
# (for example after a Slicer update). Costs a few milliseconds when it is.
# Remove with:  python3 {script} unhook
import sys as _sys
if r"{script.parent}" not in _sys.path:
    _sys.path.insert(0, r"{script.parent}")
if r"{fallback}" not in _sys.path:
    _sys.path.insert(0, r"{fallback}")
try:
    import UniNetSlicer as _uninet_setup
    _uninet_setup.ensure_at_startup()
except Exception as _uninet_error:                          # never break startup
    print("UniNet startup check failed:", _uninet_error)
del _sys
{HOOK_END}
"""


def _strip_hook(text: str) -> str:
    pattern = re.compile(
        re.escape(HOOK_BEGIN) + r".*?" + re.escape(HOOK_END) + r"\n?",
        re.S,
    )
    return pattern.sub("", text)


def install_startup_hook(slicer: Path | str | None = None) -> Path:
    """Run the check every time Slicer starts. Idempotent."""
    home = find_slicer(str(slicer) if slicer else None)
    pyslicer = _python_slicer(home)
    if pyslicer is None:
        raise SetupError(f"{home} has no bin/PythonSlicer")

    here = _script_dir()
    if here is None:
        # exec()'d from the console or fetched over HTTP: there is no file to
        # point the hook at, so keep a copy where the hook can find it.
        here = cache_dir()
        here.mkdir(parents=True, exist_ok=True)
        script = here / "UniNetSlicer.py"
        script.write_text(_own_source(), encoding="utf-8")
    else:
        script = here / "UniNetSlicer.py"
        if not script.is_file():                            # renamed copy
            script = here / Path(sys.argv[0]).name

    rcfile = slicerrc_path(home)
    text = rcfile.read_text(encoding="utf-8") if rcfile.is_file() else ""
    text = _strip_hook(text)
    if text and not text.endswith("\n"):
        text += "\n"
    text += _hook_block(Path(script), fallback_site(pyslicer))
    rcfile.parent.mkdir(parents=True, exist_ok=True)
    rcfile.write_text(text, encoding="utf-8")
    say(f"Slicer will check UniNet at every start ({rcfile})")
    return rcfile


def remove_startup_hook(slicer: Path | str | None = None) -> None:
    home = find_slicer(str(slicer) if slicer else None)
    rcfile = slicerrc_path(home)
    if not rcfile.is_file():
        say("nothing to remove")
        return
    text = rcfile.read_text(encoding="utf-8")
    stripped = _strip_hook(text)
    if stripped == text:
        say("nothing to remove")
        return
    rcfile.write_text(stripped, encoding="utf-8")
    say(f"startup check removed from {rcfile}")


def _own_source() -> str:
    """This file's own text, for the case where it was exec()'d and has no
    __file__ to copy from."""
    try:
        return Path(__file__).read_text(encoding="utf-8")
    except NameError:
        pass
    try:
        with urllib.request.urlopen(
            "https://raw.githubusercontent.com/JonasMht/UniNet/main"
            "/scripts/UniNetSlicer.py", timeout=60,
        ) as response:
            return response.read().decode("utf-8")
    except Exception as exc:                                # noqa: BLE001
        raise SetupError(
            "the startup hook needs this file on disk, but it was run without "
            "one and a copy could not be fetched: " + str(exc)
        ) from exc


# ── the entry points other code uses ─────────────────────────────────────────

def ensure(min_version: str = REQUIRED_VERSION, quiet: bool = False):
    """Return the `uninet` module, installing it into this Slicer if needed.

    Meant to be the first line of a Slicer module that needs UniNet::

        import UniNetSlicer
        uninet = UniNetSlicer.ensure()

    Costs a few milliseconds when UniNet is already there."""
    fallback = None
    try:
        home = find_slicer()
        pyslicer = _python_slicer(home)
        if pyslicer:
            fallback = str(fallback_site(pyslicer))
            if fallback not in sys.path and Path(fallback).is_dir():
                sys.path.insert(0, fallback)
    except SetupError:
        pass

    try:
        import uninet                                       # noqa: PLC0415
        if _version_tuple(uninet.__version__) >= _version_tuple(min_version):
            return uninet
        if not quiet:
            say(f"UniNet {uninet.__version__} is older than the {min_version} "
                f"this needs; upgrading")
    except ImportError:
        if not quiet:
            say("UniNet is not installed for this Slicer; installing it now")

    install()

    if fallback and fallback not in sys.path and Path(fallback).is_dir():
        sys.path.insert(0, fallback)
    import importlib                                        # noqa: PLC0415
    importlib.invalidate_caches()                           # the new dist-info
    if "uninet" in sys.modules:
        # An older build is already loaded into this process; its shared library
        # cannot be swapped out under a running interpreter.
        raise SetupError(
            "UniNet was upgraded, but the old one is already loaded in this "
            "session. Restart Slicer to use it."
        )
    import uninet                                           # noqa: PLC0415
    return uninet


def _in_slicer_gui() -> bool:
    slicer_module = sys.modules.get("slicer")
    if slicer_module is None or not hasattr(slicer_module, "app"):
        return False
    try:
        return not slicer_module.app.commandOptions().noMainWindow
    except Exception:                                       # noqa: BLE001
        return False


def ensure_at_startup() -> None:
    """What the startup hook calls. Never raises, never blocks Slicer.

    When UniNet is missing it asks before doing anything: a first build takes
    minutes, and a Slicer that silently freezes on startup is worse than one
    without UniNet. "Don't ask again" is remembered in Slicer's own settings."""
    import importlib.util                                   # noqa: PLC0415

    spec = None
    try:
        spec = importlib.util.find_spec("uninet")
    except Exception:                                       # noqa: BLE001
        pass
    if spec is not None:
        return

    if not _in_slicer_gui():
        return                                              # headless: say nothing

    import qt                                               # noqa: PLC0415
    import slicer                                           # noqa: PLC0415

    settings = slicer.app.settings()
    if settings.value("UniNet/skipStartupInstall", "false") in ("true", True):
        return

    answer = qt.QMessageBox.question(
        slicer.util.mainWindow(),
        "UniNet",
        "UniNet is not installed for this Slicer.\n\n"
        "Install it now? The first time it is built from source, which takes a "
        "few minutes; after that it is a few seconds.",
        qt.QMessageBox.Yes | qt.QMessageBox.No | qt.QMessageBox.Ignore,
    )
    if answer == qt.QMessageBox.Ignore:
        settings.setValue("UniNet/skipStartupInstall", "true")
        print("[uninet] not asking again. Undo with: "
              "slicer.app.settings().setValue('UniNet/skipStartupInstall', 'false')")
        return
    if answer != qt.QMessageBox.Yes:
        return

    dialog = qt.QProgressDialog("Installing UniNet...", "", 0, 0,
                                slicer.util.mainWindow())
    dialog.setWindowTitle("UniNet")
    dialog.setCancelButton(None)                            # a half-built install
    dialog.setMinimumWidth(560)                             # is worse than waiting
    dialog.show()
    slicer.app.processEvents()

    def progress(line: str) -> None:
        print(f"    {line}", flush=True)
        dialog.setLabelText(line[-110:])
        slicer.app.processEvents()

    try:
        result = install(on_line=progress)
    except Exception as exc:                                # noqa: BLE001
        dialog.close()
        slicer.util.errorDisplay(f"UniNet could not be installed:\n\n{exc}")
        return
    dialog.close()
    # Whether it can be used right now depends on whether anything already
    # tried to import it in this session: a failed import is cached, and a
    # module that is already loaded cannot have its shared library swapped.
    ready = (f"UniNet {result.get('version')} is installed.\n\n"
             f"Restart Slicer to use it.")
    if "uninet" not in sys.modules:                         # nothing loaded yet
        importlib.invalidate_caches()
        try:
            module = ensure(quiet=True)
            ready = f"UniNet {module.__version__} is installed and ready to use."
        except Exception:                                   # noqa: BLE001
            pass
    slicer.util.infoDisplay(ready)


def status(slicer: Path | str | None = None) -> dict:
    """Everything worth knowing, in one place, for when something is wrong."""
    report: dict = {}
    home = find_slicer(str(slicer) if slicer else None)
    report["slicer"] = str(home)
    pyslicer = _python_slicer(home)
    if pyslicer is None:
        raise SetupError(f"{home} has no bin/PythonSlicer")
    report["interpreter"] = str(pyslicer)
    info = probe(pyslicer)
    report["python"] = info["version"]
    report["site_packages"] = info["site"]
    report["site_writable"] = _site_is_writable(info)
    version, origin = installed(pyslicer)
    report["installed"] = version or "not installed"
    if origin:
        report["installed_at"] = origin
        if str(fallback_site(pyslicer)) in origin:
            # Slicer has no per-user site directory, so this copy is only
            # importable because the startup hook puts it on the path.
            report["installed_at"] += "  (needs the startup hook)"
    report["needs"] = REQUIRED_VERSION
    source = find_source()
    report["source"] = str(source) if source else "none found (would be cloned)"
    if source:
        report["source_version"] = source_version(source) or "unknown"
    wheel = find_wheel(info)
    report["prebuilt_wheel"] = str(wheel) if wheel else "none (would build)"
    report["headers"] = (
        "shipped by Slicer" if (Path(info["include"]) / "Python.h").is_file()
        else f"missing, fetched into {cache_dir()}"
    )
    report["compiler"] = _first_of("cc", "gcc", "clang") or (
        "cl.exe" if sys.platform == "win32" and shutil.which("cl.exe") else "none found"
    )
    report["startup_hook"] = (
        str(slicerrc_path(home))
        if slicerrc_path(home).is_file()
        and HOOK_BEGIN in slicerrc_path(home).read_text(encoding="utf-8")
        else "not installed"
    )
    report["cache"] = str(cache_dir())
    return report


# ── command line ─────────────────────────────────────────────────────────────

USAGE = """\
UniNet for 3D Slicer.

    python3 UniNetSlicer.py [command] [options]

Commands:
    install     install UniNet into Slicer if it is not there (the default)
    status      what is installed, where, and what would be built
    hook        also run the check every time Slicer starts
    unhook      undo that
    wheel       build a redistributable wheel and stop
    uninstall   remove UniNet from Slicer

Options:
    --slicer PATH   the Slicer installation (default: find it)
    --source PATH   the UniNet checkout to build from (default: find or clone it)
    --force         reinstall even if it is already there
    --no-build      use a prebuilt wheel or fail; never compile
"""


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    command = "install"
    options: dict = {"slicer": None, "source": None, "force": False, "build": True}

    while argv:
        arg = argv.pop(0)
        if arg in ("-h", "--help"):
            print(USAGE)
            return 0
        elif arg == "--slicer":
            options["slicer"] = argv.pop(0) if argv else None
        elif arg == "--source":
            options["source"] = argv.pop(0) if argv else None
        elif arg == "--force":
            options["force"] = True
        elif arg == "--no-build":
            options["build"] = False
        elif arg.startswith("-"):
            print(f"unknown option {arg}\n\n{USAGE}", file=sys.stderr)
            return 2
        else:
            command = arg

    try:
        if command == "status":
            for key, value in status(options["slicer"]).items():
                print(f"{key:>16} : {value}")
        elif command in ("install", "ensure"):
            result = install(options["slicer"], options["source"],
                             options["force"], options["build"])
            say(f"{result['action']}: uninet {result.get('version')} "
                f"in Slicer's Python {result['python']}")
            say("use it from a Slicer module with:  import uninet")
        elif command == "hook":
            install(options["slicer"], options["source"], options["force"],
                    options["build"])
            install_startup_hook(options["slicer"])
        elif command == "unhook":
            remove_startup_hook(options["slicer"])
        elif command == "wheel":
            wheel = make_wheel(options["slicer"], options["source"])
            say(f"copy this next to UniNetSlicer.py to make other machines "
                f"instant:\n    {wheel}")
        elif command == "uninstall":
            uninstall(options["slicer"])
        else:
            print(f"unknown command {command!r}\n\n{USAGE}", file=sys.stderr)
            return 2
    except SetupError as exc:
        print(f"\n[uninet] {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


def _console_entry() -> None:
    """What exec()'ing this file in Slicer's Python console does.

    That is the one route with no command line at all, so it does the obvious
    thing - install, then offer the startup check - rather than defining a
    dozen functions and appearing to have done nothing."""
    try:
        module = ensure()
    except SetupError as exc:
        warn(str(exc))
        return
    say(f"uninet {module.__version__} is ready in this Slicer")
    # NOT "UniNetSlicer.install_startup_hook()": exec() puts these functions
    # straight into the console's namespace, there is no module object of that
    # name, and the qualified call raises NameError.
    say("for a check at every start, run:  install_startup_hook()")


if __name__ == "__main__":
    # exec() in the Python console runs in __main__ too, but with Slicer's own
    # command line still in sys.argv. Parsing that as our arguments produced
    # "unknown option --python-code" - and worse, sys.exit() there is a request
    # to close Slicer. So which of the two this is has to be decided first.
    _slicer = sys.modules.get("slicer")
    if _slicer is not None and hasattr(_slicer, "app"):
        _console_entry()
    else:
        sys.exit(main())
