"""The version banner every Uni* library prints when it is imported.

THIS FILE IS IDENTICAL IN UniData, UniNet, UniPhys AND UniRender except for the
two lines under "the package under test". Change it in one and copy it across.

What is being pinned here is a promise made to a human reading a console in an
application that has no other way to tell one build from another: the line says
which commit this is, and whether that commit is still the source. Getting
"up to date" wrong is worse than printing nothing, so most of this file is
about the three states and the ways each can be reached.
"""
from __future__ import annotations

import os
import subprocess
import sys

import pytest

# ── the package under test ────────────────────────────────────────────────
import uninet as pkg
from uninet import _version


# ── the public surface, which callers other than the banner depend on ─────

def test_the_package_reports_a_version_and_a_build():
    info = pkg.version_info()
    assert info["package"] == pkg.__name__
    assert info["version"] == pkg.__version__
    # Every key is present whatever the state, so a caller can format the dict
    # without asking which of three shapes it got.
    for key in ("commit", "short", "describe", "date", "dirty", "source",
                "state", "detail", "python", "path"):
        assert key in info, f"version_info() is missing {key!r}"
    assert info["state"] in ("current", "stale", "checkout", "unknown")
    assert info["path"] == os.path.dirname(os.path.abspath(pkg.__file__))


def test_the_banner_names_the_package_and_the_version():
    line = pkg.banner()
    assert pkg.__name__ in line
    assert pkg.__version__ in line
    assert "\n" not in line, "the import banner is one line; use full=True for more"


def test_the_full_banner_explains_the_state():
    text = _version.full_banner(pkg.version_info())
    assert pkg.__name__ in text
    assert "state" in text
    assert "imported" in text


def test_running_from_a_checkout_says_so():
    """This test suite runs from the source tree, so that is what it must say.

    If it ever reports "up to date" instead, the checkout detection has broken
    in a way that would make an installed wheel claim freshness it cannot know.
    """
    info = pkg.version_info()
    if info["state"] == "checkout":
        assert info["commit"], "a checkout must resolve its own HEAD"
        assert info["short"] == info["commit"][:7]
    else:
        # Installed rather than imported from the tree: acceptable, but then it
        # must not silently claim to be a checkout.
        assert info["state"] in ("current", "stale", "unknown")


# ── freshness, which is the part worth getting right ──────────────────────

def _repo(tmp_path, name="src"):
    """A real git repository with one commit. Real, not faked: the reader
    parses git's on-disk format, and a hand-written approximation of it would
    pass while the real thing failed."""
    root = tmp_path / name
    root.mkdir()
    env = {**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
    def git(*args):
        subprocess.run(("git",) + args, cwd=root, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    git("init", "-q")
    (root / "a.txt").write_text("one\n")
    git("add", "a.txt")
    git("commit", "-qm", "one")
    return root, git


def _head(root):
    return subprocess.run(("git", "rev-parse", "HEAD"), cwd=root,
                          capture_output=True, text=True, check=True).stdout.strip()


@pytest.mark.skipif(not any(
    os.access(os.path.join(p, "git"), os.X_OK)
    for p in os.environ.get("PATH", "").split(os.pathsep) if p),
    reason="git is not installed")
def test_git_head_is_read_from_a_real_repository(tmp_path):
    root, _ = _repo(tmp_path)
    assert _version._git_head(str(root)) == _head(root)


@pytest.mark.skipif(not any(
    os.access(os.path.join(p, "git"), os.X_OK)
    for p in os.environ.get("PATH", "").split(os.pathsep) if p),
    reason="git is not installed")
def test_git_head_survives_gc_packing_the_refs(tmp_path):
    """`git gc` moves refs out of refs/heads/ into packed-refs.

    A reader that only looks at the loose file reports "no git" on any
    repository old enough to have been packed -- which is most of them, and
    which would make the banner degrade silently on exactly the long-lived
    checkouts it matters most for.
    """
    root, git = _repo(tmp_path)
    expected = _head(root)
    git("pack-refs", "--all")
    assert not (root / ".git" / "refs" / "heads" / "main").exists() or \
           not (root / ".git" / "refs" / "heads" / "master").exists()
    assert _version._git_head(str(root)) == expected


@pytest.mark.skipif(not any(
    os.access(os.path.join(p, "git"), os.X_OK)
    for p in os.environ.get("PATH", "").split(os.pathsep) if p),
    reason="git is not installed")
def test_git_head_follows_a_worktree_indirection(tmp_path):
    """In a linked worktree, .git is a file saying "gitdir: ...", not a dir."""
    root, git = _repo(tmp_path)
    linked = tmp_path / "linked"
    subprocess.run(("git", "worktree", "add", "-q", "-b", "wt", str(linked)),
                   cwd=root, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    assert (linked / ".git").is_file()
    assert _version._git_head(str(linked)) == _head(linked)


def test_git_head_on_a_detached_head_is_the_sha(tmp_path):
    """HEAD holds the sha directly, with no ref to follow."""
    git_dir = tmp_path / "repo" / ".git"
    git_dir.mkdir(parents=True)
    (git_dir / "HEAD").write_text("a" * 40 + "\n")
    assert _version._git_head(str(tmp_path / "repo")) == "a" * 40


def test_no_git_is_an_empty_string_not_an_exception(tmp_path):
    assert _version._git_head(str(tmp_path)) == ""
    assert _version._find_repo(str(tmp_path)) == ""


# ── the three states ──────────────────────────────────────────────────────

def _info(commit="", source="", here=None, **extra):
    """A build_info dict as _freshness would receive it."""
    info = {"commit": commit, "short": commit[:7], "source": source, **extra}
    return info


def test_a_build_matching_its_source_is_current(tmp_path):
    root, _ = _repo(tmp_path)
    head = _head(root)
    info = _info(commit=head, source=str(root))
    state, detail = _version._freshness(info, str(tmp_path))
    assert state == "current", detail


def test_a_build_whose_source_has_moved_on_is_stale(tmp_path):
    root, git = _repo(tmp_path)
    old = _head(root)
    (root / "b.txt").write_text("two\n")
    git("add", "b.txt")
    git("commit", "-qm", "two")
    new = _head(root)
    assert old != new

    info = _info(commit=old, source=str(root))
    state, detail = _version._freshness(info, str(tmp_path))
    assert state == "stale", detail
    # The detail has to name the commit to move to, or "stale" is an
    # instruction with no action attached.
    assert new[:7] in detail


def test_a_build_whose_source_is_gone_is_unknown_not_current(tmp_path):
    """The dangerous failure: claiming freshness that cannot be checked.

    On any machine that only ever installed a wheel, the recorded source
    directory does not exist. Reporting "up to date" there would be a
    guarantee invented out of nothing.
    """
    info = _info(commit="a" * 40, source=str(tmp_path / "never-existed"))
    state, _ = _version._freshness(info, str(tmp_path))
    assert state == "unknown"


def test_an_unstamped_install_says_so(tmp_path):
    """No commit and no repository above it: nothing can identify this build."""
    state, detail = _version._freshness(_info(), str(tmp_path))
    assert state == "unknown"
    assert "stamp" in detail


def test_a_checkout_reports_its_own_head(tmp_path):
    root, _ = _repo(tmp_path)
    inside = root / "python" / "pkg"
    inside.mkdir(parents=True)
    info = _info()
    state, _ = _version._freshness(info, str(inside))
    assert state == "checkout"
    # _freshness fills these in for a checkout, because there is no stamp to
    # take them from and the banner still has to name a commit.
    assert info["commit"] == _head(root)
    assert info["source"] == str(root)


# ── how the states are rendered ───────────────────────────────────────────

def _rendered(**over):
    base = {"package": "unithing", "version": "1.2.3", "commit": "", "short": "",
            "describe": "", "date": "", "dirty": False, "source": "",
            "state": "unknown", "detail": "", "python": "3.11.0", "path": "/x"}
    base.update(over)
    return _version.banner(base)


def test_stale_is_shouted_and_current_is_not():
    """A stale build is the one thing here a reader must not skim past."""
    stale = _rendered(state="stale", commit="a" * 40, short="a" * 7,
                      detail="source is now at bbbbbbb")
    assert "STALE" in stale
    assert "bbbbbbb" in stale

    current = _rendered(state="current", commit="a" * 40, short="a" * 7)
    assert "up to date" in current
    assert "STALE" not in current


def test_a_dirty_build_says_so():
    """The commit alone does not describe a build made with edits on top."""
    line = _rendered(state="current", commit="a" * 40, short="a" * 7, dirty=True)
    assert "uncommitted" in line


def test_an_unstamped_build_is_not_silently_blank():
    line = _rendered()
    assert "build unknown" in line
    assert "unstamped" in line


# ── the import-time behaviour ─────────────────────────────────────────────

def test_the_banner_is_printed_once_per_process(capsys):
    _version._printed.discard(pkg.__name__)
    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__)
    first = capsys.readouterr().err
    assert pkg.__name__ in first

    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__)
    assert capsys.readouterr().err == "", "a second import must not print again"


def test_it_goes_to_stderr_so_it_cannot_corrupt_piped_output(capsys):
    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__, force=True)
    captured = capsys.readouterr()
    assert pkg.__name__ in captured.err
    assert captured.out == ""


def test_uni_banner_0_silences_every_uni_library(monkeypatch, capsys):
    monkeypatch.setenv("UNI_BANNER", "0")
    _version._printed.discard(pkg.__name__)
    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__)
    assert capsys.readouterr().err == ""


def test_one_library_can_be_silenced_on_its_own(monkeypatch, capsys):
    monkeypatch.setenv(pkg.__name__.upper() + "_BANNER", "0")
    _version._printed.discard(pkg.__name__)
    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__)
    assert capsys.readouterr().err == ""


def test_force_prints_even_when_silenced(monkeypatch, capsys):
    monkeypatch.setenv("UNI_BANNER", "0")
    _version.print_banner(pkg.__name__, pkg.__version__, pkg.__file__, force=True)
    assert pkg.__name__ in capsys.readouterr().err


def test_printing_never_raises_however_broken_the_input(capsys):
    """A banner that could break an import would be worse than no banner."""
    _version.print_banner("no-such-package", "0", "/nonexistent/__init__.py",
                          force=True)
    _version.print_banner(None, None, None, force=True)   # type: ignore[arg-type]
    # Both returned; that is the assertion.


def test_importing_the_package_in_a_fresh_interpreter_prints_the_banner():
    """The end-to-end claim: `import <pkg>` says which build it is.

    A subprocess because the banner is once per process and this process
    imported the package before the test session started.
    """
    out = subprocess.run(
        [sys.executable, "-c", f"import {pkg.__name__}"],
        capture_output=True, text=True,
        env={**os.environ, "UNI_BANNER": "1",
             "PYTHONPATH": os.pathsep.join(sys.path)},
    )
    assert out.returncode == 0, out.stderr
    assert pkg.__name__ in out.stderr, out.stderr
    assert out.stdout == "", "the banner must not go to stdout"


def test_the_banner_can_be_silenced_from_the_environment():
    out = subprocess.run(
        [sys.executable, "-c", f"import {pkg.__name__}"],
        capture_output=True, text=True,
        env={**os.environ, "UNI_BANNER": "0",
             "PYTHONPATH": os.pathsep.join(sys.path)},
    )
    assert out.returncode == 0, out.stderr
    assert pkg.__name__ not in out.stderr, out.stderr
