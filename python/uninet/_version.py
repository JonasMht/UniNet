"""Version and build reporting, shared by every Uni* library.

THIS FILE IS IDENTICAL IN UniData, UniNet, UniPhys AND UniRender. It takes the
package's name, version and __file__ and works out the rest, so there is
nothing per-project in it and the four copies can be diffed against each other.
Change it in one and copy it to the other three; `tests/test_version.py` in
each repo checks the copy against the same expectations.

WHAT PROBLEM THIS SOLVES. A version number does not change when a fix lands, so
"0.1.0" on two machines can be three months apart, and the machine running the
older one reports nothing wrong -- it simply behaves differently. That is not a
hypothetical across this stack: the libraries are compiled extensions installed
into other applications' Pythons (3D Slicer, a Unity player, a cluster job),
where nobody sees a pip log and there is no terminal to check. The first
question of every cross-machine problem is "which build is each side running",
and until this printed, nothing anywhere answered it.

So each library says, once, when it is imported:

    uninet 0.2.0 - build 29da400 (2026-09-07) - up to date
    uniphys 0.1.0 - build 77232ae (2026-09-01) - STALE: source is now at 5a0eb23
    unidata 1.0.0 - source checkout c268d57

Three states, and the middle one is the whole point:

  up to date       built from a commit that is still the source tree's HEAD
  STALE            the source has moved on since this was built; rebuild
  source checkout  imported straight from a working tree, so it IS the source

"Up to date" is a claim about the source this was built from, not about a
release on a server: it is answerable offline, which is where these libraries
run.

Silence it with UNI_BANNER=0 (all of them) or e.g. UNINET_BANNER=0 (one).
"""
from __future__ import annotations

import os
import sys
from typing import Any, Dict, Optional, Tuple

__all__ = ["build_info", "banner", "full_banner", "print_banner"]


# ── reading git without git ────────────────────────────────────────────────
# Deliberately no subprocess. This runs at import, in hosts where spawning a
# process is slow (Windows), where git may not be installed at all (a Slicer
# release, a Unity player, a container), and where a hang would hang the
# application. Reading two small files cannot do any of that.

def _read(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return handle.read().strip()
    except OSError:
        return ""


def _git_dir(repo_root: str) -> str:
    """The .git directory for `repo_root`, resolving the worktree indirection.

    In a linked worktree or a submodule, `.git` is a FILE containing
    "gitdir: <path>" rather than a directory. Ignoring that case reports "no
    git" for anyone working in a worktree, which is exactly the sort of
    silent, environment-dependent difference this module exists to remove.
    """
    candidate = os.path.join(repo_root, ".git")
    if os.path.isdir(candidate):
        return candidate
    if os.path.isfile(candidate):
        text = _read(candidate)
        if text.startswith("gitdir:"):
            path = text[len("gitdir:"):].strip()
            if not os.path.isabs(path):
                path = os.path.join(repo_root, path)
            return os.path.normpath(path)
    return ""


def _common_dir(git_dir: str) -> str:
    """Where the REFS live for `git_dir`.

    A linked worktree has its own gitdir holding its own HEAD, but it shares
    refs, objects and packed-refs with the repository it was made from, and
    `<gitdir>/commondir` points at that. Resolving a worktree's HEAD against
    its own gitdir finds no ref file and reports "no git" -- which is what this
    did before, on exactly the checkouts a developer uses for parallel work.
    """
    text = _read(os.path.join(git_dir, "commondir"))
    if not text:
        return git_dir
    path = text.strip()
    if not os.path.isabs(path):
        path = os.path.join(git_dir, path)
    return os.path.normpath(path)


def _resolve_ref(git_dir: str, ref: str) -> str:
    """A ref name to a sha, looking in both the loose and the packed form.

    `git gc` moves refs out of refs/heads/ into packed-refs, so a reader that
    only checks the loose file works on a fresh clone and stops working a few
    weeks later -- silently, since there is no error, only an empty answer.
    """
    for base in (git_dir, _common_dir(git_dir)):
        loose = _read(os.path.join(base, ref.replace("/", os.sep)))
        if loose:
            return loose
    for base in (git_dir, _common_dir(git_dir)):
        # Lines are "<sha> <refname>"; "^<sha>" lines are peeled tags and must
        # be skipped or a tag would resolve to the object it points at.
        for line in _read(os.path.join(base, "packed-refs")).splitlines():
            if not line or line.startswith(("#", "^")):
                continue
            parts = line.split(None, 1)
            if len(parts) == 2 and parts[1].strip() == ref:
                return parts[0]
    return ""


def _git_head(repo_root: str) -> str:
    """The commit `repo_root` is on, or "" if that cannot be read.

    Handles the shapes HEAD takes: a detached sha, a symbolic ref pointing at a
    loose ref file, one whose target has been packed away by `git gc`, and one
    in a linked worktree whose refs live in the repository it was made from.
    """
    git_dir = _git_dir(repo_root)
    if not git_dir:
        return ""
    head = _read(os.path.join(git_dir, "HEAD"))
    if not head:
        return ""
    if not head.startswith("ref:"):
        return head                                   # detached: HEAD is the sha
    return _resolve_ref(git_dir, head[len("ref:"):].strip())


def _find_repo(start: str) -> str:
    """The git working tree containing `start`, or "".

    Walks up rather than assuming a fixed depth: the package lives at
    <repo>/python/<pkg>/ in a checkout but at site-packages/<pkg>/ once
    installed, and the second has no repository above it at any depth.
    """
    path = os.path.abspath(start)
    while True:
        if _git_dir(path):
            return path
        parent = os.path.dirname(path)
        if parent == path:                            # reached the filesystem root
            return ""
        path = parent


# ── the report ─────────────────────────────────────────────────────────────

def build_info(package: str, version: str, package_file: str) -> Dict[str, Any]:
    """Everything that identifies this build of `package`, as a dict.

    Meant to be printed AND sent over the wire: a peer asking "which version
    are you" should be handed this verbatim, so a mismatch between two machines
    is a comparison rather than an investigation.

    Keys are the same for every Uni* library:

      package, version   name and declared version
      commit             the commit this was built from ("" if unknown)
      short              the first 7 characters of it, for reading
      describe           `git describe --tags --always --dirty` at build time
      date               when it was built, ISO-8601
      dirty              True if the working tree had uncommitted changes then
      source             the checkout it was built from, if it was recorded
      state              "current" | "stale" | "checkout" | "unknown"
      detail             one human sentence about `state`
      python             the interpreter running it
      path               which copy of the package was actually imported
    """
    here = os.path.dirname(os.path.abspath(package_file))

    # A wheel carries _buildinfo.py, written by the build (see
    # cmake/UniBuildStamp.cmake). A source checkout does not.
    stamp: Dict[str, Any] = {}
    try:
        module = __import__(package + "._buildinfo", fromlist=["_buildinfo"])
        for key in ("BUILD_GIT", "BUILD_DESCRIBE", "BUILD_DATE", "BUILD_SOURCE",
                    "BUILD_DIRTY"):
            value = getattr(module, key, None)
            if value is not None:
                stamp[key] = value
    except Exception:      # noqa: BLE001 - an unreadable stamp must not break import
        pass

    commit = str(stamp.get("BUILD_GIT", "") or "")
    info: Dict[str, Any] = {
        "package": package,
        "version": version,
        "commit": commit,
        "short": commit[:7],
        "describe": str(stamp.get("BUILD_DESCRIBE", "") or ""),
        "date": str(stamp.get("BUILD_DATE", "") or ""),
        "dirty": bool(stamp.get("BUILD_DIRTY", False)),
        "source": str(stamp.get("BUILD_SOURCE", "") or ""),
        "python": "%d.%d.%d" % sys.version_info[:3],
        "path": here,
    }

    state, detail = _freshness(info, here)
    info["state"] = state
    info["detail"] = detail
    return info


def _freshness(info: Dict[str, Any], here: str) -> Tuple[str, str]:
    """Is this build still the source it came from? See the module docstring."""
    if not info["commit"]:
        # No stamp. Either a source checkout (then the checkout's own HEAD is
        # the honest answer) or an unstamped wheel, which is worth saying: it
        # means nothing can tell it apart from any other build of that version.
        repo = _find_repo(here)
        if repo:
            head = _git_head(repo)
            if head:
                info["commit"] = head
                info["short"] = head[:7]
                info["source"] = repo
                return "checkout", "imported from a source checkout, so it is the source"
        return "unknown", ("no build stamp: nothing can tell this apart from "
                           "any other build of the same version")

    # Stamped. Compare against the source it says it came from, if that is
    # still on this machine.
    source = info["source"] or _find_repo(here)
    if not source or not os.path.isdir(source):
        return "unknown", "the source it was built from is not on this machine"
    head = _git_head(source)
    if not head:
        return "unknown", "the source it was built from is not a git checkout"
    if head == info["commit"]:
        return "current", "built from the current source"
    return "stale", "source is now at " + head[:7]


# ── rendering ──────────────────────────────────────────────────────────────

def banner(info: Dict[str, Any]) -> str:
    """The one line printed at import."""
    parts = ["%(package)s %(version)s" % info]

    if info["state"] == "checkout":
        parts.append("source checkout " + (info["short"] or "?"))
    elif info["short"]:
        built = "build " + info["short"]
        if info["date"]:
            built += " (" + info["date"][:10] + ")"
        if info["dirty"]:
            built += " +uncommitted"
        parts.append(built)
    else:
        parts.append("build unknown")

    if info["state"] == "current":
        parts.append("up to date")
    elif info["state"] == "stale":
        parts.append("STALE: " + info["detail"])
    elif info["state"] == "unknown" and not info["short"]:
        parts.append("unstamped")

    return " - ".join(parts)


def full_banner(info: Dict[str, Any]) -> str:
    """Several lines, for a bug report or an explicit print_banner(full=True)."""
    lines = [banner(info)]
    if info["describe"]:
        lines.append("  describe   " + info["describe"])
    if info["commit"]:
        lines.append("  commit     " + info["commit"])
    lines.append("  state      %s (%s)" % (info["state"], info["detail"]))
    if info["source"]:
        lines.append("  source     " + info["source"])
    lines.append("  imported   " + info["path"])
    lines.append("  python     " + info["python"])
    return "\n".join(lines)


# Printed once per process per package. A library imported from three places
# should say so once, and one that is never imported should say nothing.
_printed = set()


def print_banner(package: str, version: str, package_file: str, *,
                 force: bool = False, full: bool = False, stream=None) -> None:
    """Say which build this is, once.

    Called from each package's __init__, which is the one moment every user of
    the library passes through exactly once.

    Silenced by UNI_BANNER=0 for every Uni* library at once, or by
    <PACKAGE>_BANNER=0 for one of them -- so a script that imports four of them
    can quiet the three it does not care about. `force=True` ignores both,
    because an explicit call is a request rather than a side effect.

    Never raises. A banner that could break an import would be worse than no
    banner: the whole point is to be a line of output, not a dependency.
    """
    try:
        if not force:
            if package in _printed:
                return
            if os.environ.get("UNI_BANNER", "1") == "0":
                return
            if os.environ.get(package.upper() + "_BANNER", "1") == "0":
                return
        _printed.add(package)
        info = build_info(package, version, package_file)
        text = full_banner(info) if full else banner(info)
        # stderr by default: this is diagnostic output, and a library that
        # writes to stdout corrupts any program whose stdout is data -- a
        # rendered image, a CBOR stream, a JSON report piped to another tool.
        print(text, file=stream if stream is not None else sys.stderr, flush=True)
    except Exception:      # noqa: BLE001
        pass


def _optional(package: str, version: str, package_file: str) -> Optional[Dict[str, Any]]:
    """build_info() that returns None instead of raising. Used by callers that
    report versions as data (a version message on the network, a run header)
    and must not fail because provenance could not be worked out."""
    try:
        return build_info(package, version, package_file)
    except Exception:      # noqa: BLE001
        return None
