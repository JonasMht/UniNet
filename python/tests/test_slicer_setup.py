"""Tests for scripts/UniNetSlicer.py, the 3D Slicer installer.

None of these need Slicer, a compiler or a network. What they cover is the part
of the installer that decides *what to do*: where Slicer is, which interpreter
that means, whether a prebuilt wheel fits, what goes into .slicerrc.py and what
the build environment ends up being. Those decisions are the ones that used to
be made by hand, wrongly, and they are cheap to check.

The build itself is not tested here: it needs a real Slicer, and it is covered
by running the installer against one (scripts/test-slicer-setup.sh).

    pytest python/tests/test_slicer_setup.py -v
"""
from __future__ import annotations

import re
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

import UniNetSlicer as setup                                # noqa: E402

WINDOWS = sys.platform == "win32"

# This file promises to need no compiler. Four tests do need one - they ask the
# installer which compiler it would use - so they say so rather than failing on
# a machine that has none.
import shutil                                              # noqa: E402
NO_COMPILER = not any(shutil.which(name) for name in ("cc", "gcc", "clang", "cl.exe"))
needs_compiler = pytest.mark.skipif(NO_COMPILER, reason="no C compiler on this machine")


# ── a Slicer that is not Slicer ──────────────────────────────────────────────

def make_fake_slicer(root: Path, name: str = "Slicer-5.8.1-linux-amd64",
                     no_site: bool = False) -> Path:
    """A directory shaped like a Slicer installation, whose PythonSlicer is a
    proxy for the interpreter running the tests. Enough for every decision the
    installer makes before it builds anything.

    `no_site` starts that proxy with -S, which is how a Slicer that does not
    have UniNet installed is imitated on a machine that does."""
    home = root / name
    (home / "bin").mkdir(parents=True)
    launcher = home / "bin" / "PythonSlicer"
    flag = " -S" if no_site else ""
    launcher.write_text(f'#!/bin/sh\nexec "{sys.executable}"{flag} "$@"\n', encoding="utf-8")
    launcher.chmod(0o755)
    return home


@pytest.fixture()
def fake_slicer(tmp_path: Path) -> Path:
    if WINDOWS:
        pytest.skip("the proxy launcher is a shell script")
    return make_fake_slicer(tmp_path)


@pytest.fixture(autouse=True)
def isolated_env(tmp_path, monkeypatch):
    """No test may read or write the developer's real cache, home directory or
    Slicer installation."""
    monkeypatch.setenv("UNINET_CACHE", str(tmp_path / "cache"))
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.setenv("USERPROFILE", str(tmp_path / "home"))
    (tmp_path / "home").mkdir(exist_ok=True)
    for name in ("UNINET_SLICER", "UNINET_SOURCE", "UNINET_WHEEL", "SLICERRC"):
        monkeypatch.delenv(name, raising=False)


# ── finding Slicer ───────────────────────────────────────────────────────────

def test_explicit_path_wins(fake_slicer):
    assert setup.find_slicer(str(fake_slicer)) == fake_slicer


def test_environment_variable_is_honoured(fake_slicer, monkeypatch):
    monkeypatch.setenv("UNINET_SLICER", str(fake_slicer))
    assert setup.find_slicer() == fake_slicer


def test_a_directory_that_is_not_slicer_says_so(tmp_path):
    with pytest.raises(setup.SetupError) as error:
        setup.find_slicer(str(tmp_path))
    # The message has to name what was missing, or the user retries the same
    # wrong path: "not a Slicer installation" alone does not say why.
    assert "bin/PythonSlicer" in str(error.value)


def test_no_slicer_anywhere_is_an_error_with_instructions(tmp_path, monkeypatch):
    monkeypatch.setattr(setup, "_slicer_candidates", lambda: [])
    monkeypatch.setattr(sys, "prefix", str(tmp_path))
    with pytest.raises(setup.SetupError) as error:
        setup.find_slicer()
    assert "--slicer" in str(error.value)


@pytest.mark.skipif(WINDOWS, reason="the proxy launcher is a shell script")
def test_the_newest_installation_is_chosen(tmp_path, monkeypatch):
    home = tmp_path / "home"
    make_fake_slicer(home, "Slicer-5.6.2-linux-amd64")
    newest = make_fake_slicer(home, "Slicer-5.8.1-linux-amd64")
    make_fake_slicer(home, "Slicer-5.8.0-linux-amd64")
    monkeypatch.setattr(sys, "platform", "linux")
    candidates = setup._slicer_candidates()
    assert candidates[0] == newest, candidates


def test_running_under_slicers_own_python_finds_the_tree(fake_slicer, monkeypatch):
    # sys.prefix inside Slicer points at lib/Python, several levels down.
    prefix = fake_slicer / "lib" / "Python"
    prefix.mkdir(parents=True)
    monkeypatch.setattr(sys, "prefix", str(prefix))
    monkeypatch.setattr(setup, "_slicer_candidates", lambda: [])
    assert setup.find_slicer() == fake_slicer


# ── asking that Python about itself ──────────────────────────────────────────

def test_probe_reports_the_proxied_interpreter(fake_slicer):
    info = setup.probe(fake_slicer / "bin" / "PythonSlicer")
    assert info["version"].startswith("%d.%d" % sys.version_info[:2])
    # The purelib directory of the proxied interpreter, whatever that
    # interpreter is: "site-packages" on most builds, "dist-packages" on
    # Debian-family system Pythons. Both are honest answers, and the probe is
    # only ever asked what "this Python" reports about itself.
    site = Path(info["site"])
    assert info["site"] and (
        site.name in ("site-packages", "dist-packages"))
    assert site.is_dir()


def test_probe_on_something_unrunnable_explains(tmp_path):
    broken = tmp_path / "PythonSlicer"
    broken.write_text("#!/bin/sh\nexit 3\n", encoding="utf-8")
    broken.chmod(0o755)
    with pytest.raises(setup.SetupError):
        setup.probe(broken)


def test_a_missing_uninet_is_reported_as_missing(tmp_path):
    if WINDOWS:
        pytest.skip("the proxy launcher is a shell script")
    home = make_fake_slicer(tmp_path / "bare", no_site=True)
    version, origin = setup.installed(home / "bin" / "PythonSlicer")
    assert (version, origin) == (None, None)


# ── choosing a wheel ─────────────────────────────────────────────────────────

def test_a_wheel_for_another_python_is_not_used(tmp_path, monkeypatch):
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    _stamped_wheel(tmp_path, "uninet-0.2.0-cp312-cp312-linux_x86_64.whl", None)
    monkeypatch.setenv("UNINET_WHEEL", str(wheels))
    assert setup.find_wheel({"xy": "3.9"}) is None


def test_a_wheel_for_this_python_is_used(tmp_path, monkeypatch):
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    wanted = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", None)
    monkeypatch.setenv("UNINET_WHEEL", str(wheels))
    assert setup.find_wheel({"xy": "3.9"}) == wanted


def test_a_wheel_given_as_a_file_is_used_as_is(tmp_path, monkeypatch):
    wheel = tmp_path / "whatever-name.whl"
    wheel.write_bytes(b"")
    monkeypatch.setenv("UNINET_WHEEL", str(wheel))
    assert setup.find_wheel({"xy": "3.9"}) == wheel


def test_the_repository_is_searched_for_a_wheel(tmp_path, monkeypatch):
    dist = tmp_path / "checkout" / "dist"
    dist.mkdir(parents=True)
    wheel = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", None)
    wheel.rename(dist / wheel.name)
    monkeypatch.setattr(setup, "find_source", lambda: tmp_path / "checkout")
    monkeypatch.setattr(setup, "_script_dir", lambda: None)
    assert setup.find_wheel({"xy": "3.9"}) == dist / wheel.name


# ── integrity: a broken wheel must never be preferred over building ─────────

def test_a_truncated_wheel_is_not_used_with_or_without_a_checkout(tmp_path, monkeypatch):
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    junk = wheels / "uninet-0.2.0-cp39-cp39-linux_x86_64.whl"
    junk.write_bytes(b"half a wheel, cut off mid-download")
    monkeypatch.setenv("UNINET_WHEEL", str(wheels))
    skipped: list = []
    # no git source at all: still must not trust it
    assert setup.find_wheel({"xy": "3.9"},
                            on_skip=lambda w, d: skipped.append(w)) is None
    assert [Path(w).name for w in skipped] == [junk.name]
    # with a checkout: same refusal, and the reason is reported, never a crash
    skipped.clear()
    monkeypatch.setattr(setup, "_source_commit",
                        lambda source: "9" * 40 if source else "")
    assert setup.find_wheel({"xy": "3.9"}, tmp_path / "checkout",
                            on_skip=lambda w, d: skipped.append(w)) is None
    assert [Path(w).name for w in skipped] == [junk.name]


def test_a_real_wheel_that_survives_the_zip_check_is_usable(tmp_path, monkeypatch):
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    good = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", "5" * 40)
    monkeypatch.setenv("UNINET_WHEEL", str(wheels))
    on_skip = lambda w, d: (_ for _ in ()).throw(AssertionError(f"{w} was skipped"))
    assert setup.find_wheel({"xy": "3.9"}, on_skip=on_skip) == good


# ── repair: a wheel that lost its stamp is fixed, RECORD regenerated ────────

def _wheel_with_record(root: Path, name: str, stamp: str | None) -> Path:
    """A wheel shaped like one pip actually built: registered members (none
    for _buildinfo yet) and a dist-info/RECORD that lists them."""
    import io                                                               # noqa: PLC0415
    wheels = root / "wheels"
    wheels.mkdir(exist_ok=True)
    path = wheels / name
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("uninet/__init__.py", "__version__ = '0.2.0'\n")
        record = (
            "uninet/__init__.py,"
            + setup._sha256_urlsafe(b"__version__ = '0.2.0'\n")
            + ",24\n"
            "uninet-0.2.0.dist-info/RECORD,,\n"
        )
        archive.writestr("uninet-0.2.0.dist-info/RECORD", record)
    path.write_bytes(buffer.getvalue())
    return path


def test_a_wheel_that_lost_its_stamp_is_repaired_in_place(tmp_path):
    wheel = _wheel_with_record(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", None)
    before = wheel.read_bytes()
    assert setup._wheel_commit(wheel) == ""
    assert setup._ensure_wheel_stamp(wheel, "a" * 40) is True
    assert setup._wheel_commit(wheel) == "a" * 40
    # the wheel is still a real, installable zip
    with zipfile.ZipFile(wheel) as zf:
        assert zf.testzip() is None
        members = zf.namelist()
        assert "uninet/_buildinfo.py" in members
        # RECORD was regenerated and now covers the stamped member too,
        # so `pip uninstall` removes every file and leaves nothing behind.
        record = zf.read("uninet-0.2.0.dist-info/RECORD").decode()
        lines = [line for line in record.splitlines() if line.strip()]
        assert "uninet/_buildinfo.py," in record
        for line in lines[:-1]:
            path, digest, size = line.rsplit(",", 2)
            data = zf.read(path)
            assert setup._sha256_urlsafe(data) == digest, f"bad RECORD line: {line}"
            assert int(size) == len(data)
        assert wheel.read_bytes() != before


def test_stamping_a_wheel_that_already_matches_is_a_noop(tmp_path):
    wheel = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", "b" * 40)
    before = wheel.read_bytes()
    assert setup._ensure_wheel_stamp(wheel, "b" * 40) is True
    assert wheel.read_bytes() == before      # untouched: nothing to repair


def test_repair_fails_cleanly_on_a_file_that_is_not_a_wheel(tmp_path):
    junk = tmp_path / "not-a-wheel.whl"
    junk.write_bytes(b"raw bytes")
    assert setup._ensure_wheel_stamp(junk, "c" * 40) is False     # no exception


# ── build provenance: refusing a wheel that predates the source ──────────────

def _stamped_wheel(root: Path, name: str, commit: str | None) -> Path:
    """A real zip wheel for the parts of the installer that inspect it. The
    stamp goes in uninet/_buildinfo.py exactly where a wheel build would put
    it; None means a wheel from before stamping existed."""
    import io                                                               # noqa: PLC0415
    import zipfile                                                          # noqa: PLC0415
    wheels = root / "wheels"
    wheels.mkdir(exist_ok=True)
    path = wheels / name
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        if commit is not None:
            archive.writestr("uninet/_buildinfo.py",
                             f'BUILD_GIT = "{commit}"\n')
    path.write_bytes(buffer.getvalue())
    return path


def test_a_wheel_stamped_with_the_current_source_is_reused(tmp_path, monkeypatch):
    commit = "1" * 40
    monkeypatch.setattr(setup, "_source_commit",
                        lambda source: commit if source else "")
    wanted = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", commit)
    monkeypatch.setenv("UNINET_WHEEL", str(wanted.parent))
    skipped: list = []
    assert setup.find_wheel({"xy": "3.9"}, tmp_path / "checkout",
                            on_skip=lambda w, d: skipped.append(w)) == wanted
    assert skipped == []


def test_a_wheel_from_an_older_source_is_refused_and_said_so(tmp_path, monkeypatch):
    current = "2" * 40
    monkeypatch.setattr(setup, "_source_commit",
                        lambda source: current if source else "")
    old = _stamped_wheel(
        tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64_old.whl", "1" * 40)
    good = _stamped_wheel(
        tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", current)
    monkeypatch.setenv("UNINET_WHEEL", str(old.parent))
    skipped: list = []
    assert setup.find_wheel({"xy": "3.9"}, tmp_path / "checkout",
                            on_skip=lambda w, d: skipped.append(w)) == good
    assert [Path(w).name for w in skipped] == [old.name]


def test_an_unstamped_wheel_is_not_trusted_when_a_checkout_exists(tmp_path, monkeypatch):
    monkeypatch.setattr(setup, "_source_commit",
                        lambda source: "3" * 40 if source else "")
    wheel = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", None)
    monkeypatch.setenv("UNINET_WHEEL", str(wheel.parent))
    skipped: list = []
    assert setup.find_wheel({"xy": "3.9"}, tmp_path / "checkout",
                            on_skip=lambda w, d: skipped.append(w)) is None
    assert [Path(w).name for w in skipped] == [wheel.name]


def test_an_unstamped_wheel_is_used_when_there_is_no_checkout(tmp_path, monkeypatch):
    """With no git source at hand there is nothing to compare against, and a
    pre-stamping wheel is all a colleague with a file-only workflow has: using
    it is the legacy behaviour and must not regress."""
    wheel = _stamped_wheel(tmp_path, "uninet-0.2.0-cp39-cp39-linux_x86_64.whl", None)
    monkeypatch.setenv("UNINET_WHEEL", str(wheel.parent))
    assert setup.find_wheel({"xy": "3.9"}) == wheel


# ── versions ─────────────────────────────────────────────────────────────────

def test_versions_compare_numerically_not_as_text():
    # "0.10.0" < "0.9.0" as strings, which would refuse to upgrade.
    assert setup._version_tuple("0.10.0") > setup._version_tuple("0.9.0")
    assert setup._version_tuple("0.2.0") == setup._version_tuple("0.2.0")


def test_the_source_version_is_read_from_pyproject(tmp_path):
    (tmp_path / "pyproject.toml").write_text(
        '[project]\nname = "uninet"\nversion = "1.2.3"\n', encoding="utf-8")
    assert setup.source_version(tmp_path) == "1.2.3"


def test_this_file_and_pyproject_agree():
    """A REQUIRED_VERSION left behind after a release would either force a
    pointless rebuild on every start or accept a version that is too old."""
    root = Path(__file__).resolve().parents[2]
    assert setup.source_version(root) == setup.REQUIRED_VERSION


def test_every_packaged_version_declaration_agrees():
    """The version lives in five places because five different package formats
    read it from their own file: C++ (CMakeLists.txt), the Python wheel
    (pyproject.toml and the module), the Slicer installer (REQUIRED_VERSION)
    and C# (the csproj's <Version>). Nothing stops them drifting except this
    test and scripts/release.py - and a release where they disagree is a
    release where the Slicer installer and pip serve '0.2.0' that mean
    different things.
    scripts/release.py bump makes all five agree at once."""
    root = Path(__file__).resolve().parents[2]
    cmake = re.search(
        r'project\(UniNet\s+LANGUAGES C CXX VERSION\s+([0-9][^ )]*)',
        (root / "CMakeLists.txt").read_text(encoding="utf-8"))
    pyproject = re.search(
        r'^\s*version\s*=\s*"([^"]+)"',
        (root / "pyproject.toml").read_text(encoding="utf-8"),
        re.M)
    init = re.search(
        r'__version__\s*=\s*"([^"]+)"',
        (root / "python" / "uninet" / "__init__.py").read_text(encoding="utf-8"))
    csproj = re.search(
        r'<Version>([^<]+)</Version>',
        (root / "csharp" / "UniNet" / "UniNet.csproj").read_text(encoding="utf-8"))

    declared = {
        "CMakeLists.txt": cmake.group(1) if cmake else "",
        "pyproject.toml": pyproject.group(1) if pyproject else "",
        "python/uninet/__init__.py": init.group(1) if init else "",
        "csharp/UniNet/UniNet.csproj": csproj.group(1) if csproj else "",
        "scripts/UniNetSlicer.py (REQUIRED_VERSION)": setup.REQUIRED_VERSION,
    }
    reference = declared["pyproject.toml"]
    assert reference, "pyproject.toml must carry a version"
    mismatched = {path: value for path, value in declared.items()
                  if value != reference}
    assert not mismatched, (
        "version declarations disagree with pyproject.toml: "
        + ", ".join(f"{path}={value}" for path, value in mismatched.items()))


# ── the startup hook ─────────────────────────────────────────────────────────

def test_the_hook_is_written_and_removed_leaving_the_rest_alone(fake_slicer, monkeypatch, tmp_path):
    rcfile = tmp_path / "home" / ".slicerrc.py"
    rcfile.write_text("print('my own startup code')\n", encoding="utf-8")

    setup.install_startup_hook(fake_slicer)
    text = rcfile.read_text(encoding="utf-8")
    assert "my own startup code" in text
    assert setup.HOOK_BEGIN in text and setup.HOOK_END in text
    assert "ensure_at_startup" in text

    setup.remove_startup_hook(fake_slicer)
    assert rcfile.read_text(encoding="utf-8") == "print('my own startup code')\n"


def test_installing_the_hook_twice_leaves_one_copy(fake_slicer, tmp_path):
    rcfile = tmp_path / "home" / ".slicerrc.py"
    setup.install_startup_hook(fake_slicer)
    setup.install_startup_hook(fake_slicer)
    assert rcfile.read_text(encoding="utf-8").count(setup.HOOK_BEGIN) == 1


def test_the_hook_goes_where_slicer_will_actually_read_it(fake_slicer, monkeypatch, tmp_path):
    """Slicer reads .slicerrc.py from its own directory in preference to the
    one in $HOME. Writing to $HOME while that exists produces a hook that never
    runs, and nothing says so."""
    assert setup.slicerrc_path(fake_slicer) == Path(tmp_path / "home" / ".slicerrc.py")

    monkeypatch.setenv("SLICERRC", str(tmp_path / "elsewhere.py"))
    assert setup.slicerrc_path(fake_slicer) == tmp_path / "elsewhere.py"

    local = fake_slicer / ".slicerrc.py"
    local.write_text("", encoding="utf-8")
    assert setup.slicerrc_path(fake_slicer) == local


def test_the_hook_survives_being_read_by_python(fake_slicer, tmp_path):
    """The block is executed by Slicer at startup, so it has to be valid Python
    even on a machine where nothing else is set up."""
    setup.install_startup_hook(fake_slicer)
    text = (tmp_path / "home" / ".slicerrc.py").read_text(encoding="utf-8")
    compile(text, "slicerrc", "exec")


def test_a_hand_edited_rc_file_is_not_duplicated(fake_slicer, tmp_path):
    rcfile = tmp_path / "home" / ".slicerrc.py"
    setup.install_startup_hook(fake_slicer)
    text = rcfile.read_text(encoding="utf-8")
    rcfile.write_text(text + "\nprint('after')\n", encoding="utf-8")
    setup.install_startup_hook(fake_slicer)
    assert rcfile.read_text(encoding="utf-8").count(setup.HOOK_BEGIN) == 1
    assert "print('after')" in rcfile.read_text(encoding="utf-8")


def test_removing_a_hook_that_is_not_there_is_not_an_error(fake_slicer):
    setup.remove_startup_hook(fake_slicer)


# ── the build environment ────────────────────────────────────────────────────

def test_the_parent_interpreters_environment_does_not_leak(monkeypatch):
    """Slicer's Python inheriting our PYTHONHOME imports half of one
    installation and half of another; a compiler inheriting Slicer's
    LD_LIBRARY_PATH links against Slicer's bundled libraries."""
    monkeypatch.setenv("PYTHONHOME", "/somewhere/else")
    monkeypatch.setenv("PYTHONPATH", "/somewhere/else")
    monkeypatch.setenv("LD_LIBRARY_PATH", "/opt/Slicer/lib")
    env = setup.child_env()
    assert "PYTHONHOME" not in env
    assert "PYTHONPATH" not in env
    assert "LD_LIBRARY_PATH" not in env


@needs_compiler
@pytest.mark.skipif(WINDOWS, reason="MSVC is found by cmake, not by CC")
def test_the_compiler_is_named_explicitly(monkeypatch):
    """Slicer's Python remembers being built by devtoolset-7 and exports that
    as CC. cmake then looks for a compiler that exists on no user's machine."""
    monkeypatch.setenv("CC", "/opt/rh/devtoolset-7/root/usr/bin/gcc")
    chosen = setup.find_compiler()
    assert Path(chosen["CC"]).exists()
    assert Path(chosen["CXX"]).exists()


@needs_compiler
@pytest.mark.skipif(WINDOWS, reason="MSVC is found by cmake, not by CC")
def test_the_compiler_can_be_chosen(monkeypatch):
    monkeypatch.setenv("UNINET_CC", "/usr/bin/clang")
    monkeypatch.setenv("UNINET_CXX", "/usr/bin/clang++")
    assert setup.find_compiler()["CC"] == "/usr/bin/clang"


def test_headers_shipped_by_slicer_are_used_unchanged(tmp_path):
    include = tmp_path / "include"
    include.mkdir()
    (include / "Python.h").write_text("", encoding="utf-8")
    assert setup.ensure_headers({"include": str(include), "version": "3.9.10"}) is None


@needs_compiler
def test_the_header_shim_reaches_every_variable_cmake_might_read(tmp_path, monkeypatch, fake_slicer):
    """pybind11 reads Python3_INCLUDE_DIR, scikit-build-core sets
    Python_INCLUDE_DIR from sysconfig (which is the directory that is missing
    the headers), and the legacy FindPythonLibsNew path reads neither."""
    shim = tmp_path / "shim"
    shim.mkdir()
    monkeypatch.setattr(setup, "ensure_headers", lambda info: shim)
    env = setup._build_env(fake_slicer / "bin" / "PythonSlicer", {"xy": "3.9"})
    assert f"-DPython_INCLUDE_DIR={shim}" in env["CMAKE_ARGS"]
    assert f"-DPython3_INCLUDE_DIR={shim}" in env["CMAKE_ARGS"]
    assert f"-I{shim}" in env["CXXFLAGS"]
    assert f"-I{shim}" in env["CFLAGS"]
    # Out of the source tree: the checkout may be read-only or shared.
    assert str(setup.cache_dir()) in env["SKBUILD_BUILD_DIR"]


@needs_compiler
def test_the_wheel_is_built_to_be_portable(monkeypatch, fake_slicer):
    """A wheel linked against a system libzyre imports only on machines that
    also have it, and the error arrives on the colleague's machine, naming a
    library they never installed. Also pin cmake: 4.x refuses to configure
    ZeroMQ, czmq and zyre, which still ask for compatibility with CMake 2.8."""
    monkeypatch.setattr(setup, "ensure_headers", lambda info: None)
    env = setup._build_env(fake_slicer / "bin" / "PythonSlicer", {"xy": "3.9"})
    assert "-DUNINET_SELF_CONTAINED=ON" in env["CMAKE_ARGS"]
    assert env["SKBUILD_CMAKE_VERSION"] == ">=3.15,<4"


@needs_compiler
def test_an_existing_cmake_args_is_added_to_not_replaced(tmp_path, monkeypatch, fake_slicer):
    shim = tmp_path / "shim"
    shim.mkdir()
    monkeypatch.setattr(setup, "ensure_headers", lambda info: shim)
    monkeypatch.setenv("CMAKE_ARGS", "-DUNINET_LZ4=OFF")
    env = setup._build_env(fake_slicer / "bin" / "PythonSlicer", {"xy": "3.9"})
    assert "-DUNINET_LZ4=OFF" in env["CMAKE_ARGS"]
    assert "-DPython3_INCLUDE_DIR" in env["CMAKE_ARGS"]


# ── the older way of installing ──────────────────────────────────────────────

def test_a_hand_copied_install_is_removed_before_installing(tmp_path):
    """The previous build-for-slicer.sh copied files into site-packages. pip
    cannot see such an install, cannot remove it, and it shadows the new one -
    so a rebuild appears to change nothing."""
    site = tmp_path / "site-packages"
    (site / "uninet").mkdir(parents=True)
    (site / "uninet" / "__init__.py").write_text("", encoding="utf-8")
    setup._remove_legacy_copy({"site": str(site)})
    assert not (site / "uninet").exists()


def test_a_pip_install_is_left_for_pip_to_replace(tmp_path):
    site = tmp_path / "site-packages"
    (site / "uninet").mkdir(parents=True)
    (site / "uninet-0.2.0.dist-info").mkdir()
    setup._remove_legacy_copy({"site": str(site)})
    assert (site / "uninet").exists()


# ── handing the wheel to somebody else ───────────────────────────────────────

def test_a_wheel_needing_a_library_from_usr_local_is_flagged(tmp_path, monkeypatch):
    """A wheel built on a machine with libraries in /usr/local imports there and
    nowhere else, and the failure surfaces on the colleague's machine."""
    if WINDOWS or sys.platform == "darwin":
        pytest.skip("ldd is Linux")
    import zipfile
    wheel = tmp_path / "uninet-0.2.0-cp39-cp39-linux_x86_64.whl"
    with zipfile.ZipFile(wheel, "w") as archive:
        archive.writestr("uninet/_uninet.so", b"not really an ELF file")
    monkeypatch.setattr(setup.subprocess, "run", lambda *a, **k: type(
        "R", (), {"stdout": "\tliblz4.so.1 => /usr/local/lib/liblz4.so.1 (0x00007f)\n"
                            "\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f)\n"})())
    warnings = setup._portability_warnings(wheel)
    assert warnings and "/usr/local/lib/liblz4.so.1" in warnings[0]


def test_a_wheel_needing_only_system_libraries_is_not_flagged(tmp_path, monkeypatch):
    if WINDOWS or sys.platform == "darwin":
        pytest.skip("ldd is Linux")
    import zipfile
    wheel = tmp_path / "uninet-0.2.0-cp39-cp39-linux_x86_64.whl"
    with zipfile.ZipFile(wheel, "w") as archive:
        archive.writestr("uninet/_uninet.so", b"not really an ELF file")
    monkeypatch.setattr(setup.subprocess, "run", lambda *a, **k: type(
        "R", (), {"stdout": "\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f)\n"})())
    assert setup._portability_warnings(wheel) == []
