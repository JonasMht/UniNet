#!/usr/bin/env bash
# Prove the C# binding survives IL2CPP, which is what Unity uses for Android and
# iOS. Run it after touching anything in csharp/UniNet/.
#
#     ./scripts/check-il2cpp.sh [/path/to/Unity/Hub/Editor/<version>]
#
# WHAT THIS CATCHES. IL2CPP is ahead-of-time: no JIT, so a managed method that
# native code calls back into must exist as a real function at compile time. Only
# a static method carrying [MonoPInvokeCallback] qualifies. Register a lambda
# instead and everything still compiles, the editor still works, and the device
# throws on the first message:
#
#     NotSupportedException: To marshal a managed method, please add an
#     attribute named 'MonoPInvokeCallback' to the method definition.
#
# Nothing in an ordinary build or test run notices. This does: it compiles the
# binding against Unity's AOT class library and runs the real il2cpp compiler
# over it, then asserts that a ReversePInvokeWrapper_ was emitted for every
# callback. A lambda emits none, so the count is the whole test.
#
# No Unity licence is needed: il2cpp is a standalone compiler.
set -euo pipefail

UNITY="${1:-}"
if [ -z "$UNITY" ]; then
    UNITY="$(ls -d "$HOME"/Unity/Hub/Editor/* 2>/dev/null | sort -V | tail -1)"
fi
DATA="$UNITY/Editor/Data"
IL2CPP="$DATA/il2cpp/build/deploy/il2cpp"
CSC="$DATA/DotNetSdkRoslyn/csc.dll"
AOT="$DATA/MonoBleedingEdge/lib/mono/unityaot-linux"
MANAGED="$DATA/PlaybackEngines/AndroidPlayer/Variations/il2cpp/Managed"

if [ ! -x "$IL2CPP" ] || [ ! -f "$CSC" ] || [ ! -d "$AOT" ]; then
    cat >&2 <<EOF
usage: $0 [/path/to/Unity/Hub/Editor/<version>]

Needs a Unity install with Android Build Support. Looked in:
  il2cpp : $IL2CPP
  csc    : $CSC
  bcl    : $AOT
EOF
    exit 2
fi
command -v dotnet >/dev/null || { echo "the .NET SDK is required to run csc" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/asm"

echo "Unity : $UNITY"

# UNITY_5_3_OR_NEWER so the sources use Unity's own MonoPInvokeCallback rather
# than the fallback declaration in Native.cs: this must test what Unity compiles.
REFS=()
for f in "$AOT"/*.dll "$AOT"/Facades/*.dll; do REFS+=("-r:$f"); done
[ -f "$MANAGED/UnityEngine.CoreModule.dll" ] && REFS+=("-r:$MANAGED/UnityEngine.CoreModule.dll")

echo "compiling the binding against Unity's AOT class library..."
dotnet "$CSC" -nologo -target:library -out:"$WORK/asm/UniNet.dll" -nostdlib+ \
    -langversion:9.0 -define:UNITY_5_3_OR_NEWER -nullable:enable -optimize+ \
    "${REFS[@]}" "$HERE"/csharp/UniNet/*.cs

# il2cpp wants the whole reference closure present up front and names one missing
# assembly per run, so this converges by adding whichever it asks for next.
echo "resolving the assembly closure..."
for base in mscorlib System System.Core System.Xml System.Configuration; do
    cp "$AOT/$base.dll" "$WORK/asm/" 2>/dev/null || true
done
cp "$AOT/Facades/netstandard.dll" "$WORK/asm/" 2>/dev/null || true
for m in UnityEngine.CoreModule UnityEngine.SharedInternalsModule; do
    cp "$MANAGED/$m.dll" "$WORK/asm/" 2>/dev/null || true
done

for _ in $(seq 1 25); do
    rm -rf "$WORK/cpp"
    OUT="$("$IL2CPP" --convert-to-cpp --directory="$WORK/asm" --generatedcppdir="$WORK/cpp" \
           --dotnetprofile=unityaot-linux --platform=Android --architecture=ARM64 2>&1)" || true
    # `|| true` is load-bearing: grep exits 1 when it matches nothing, and under
    # `set -e` that status propagates out of the assignment and kills the script
    # at the exact moment the closure is complete, i.e. on success.
    MISSING="$(grep -oP '[A-Za-z0-9._]+(?= was not resolved up front)' <<<"$OUT" | sort -u || true)"
    [ -z "$MISSING" ] && break
    for m in $MISSING; do
        cp "$AOT/$m.dll" "$WORK/asm/" 2>/dev/null \
            || cp "$AOT/Facades/$m.dll" "$WORK/asm/" 2>/dev/null \
            || cp "$MANAGED/$m.dll" "$WORK/asm/" 2>/dev/null \
            || { echo "cannot locate $m.dll" >&2; exit 1; }
    done
done

if grep -q '^Error:' <<<"${OUT:-}"; then
    echo "il2cpp failed to convert the binding:" >&2
    head -20 <<<"$OUT" >&2
    exit 1
fi

# Every callback UniNet registers with the C ABI, and the type it belongs to.
EXPECTED=(
    "Session_OnPeerFound" "Session_OnPeerLost" "Session_OnJson" "Session_OnCbor"
    "Blob_OnReceived" "Blob_OnProgress" "Blob_OnFailed"
)
echo
FAIL=0
for name in "${EXPECTED[@]}"; do
    if grep -rqoP "ReversePInvokeWrapper_${name}_\w+" "$WORK/cpp/"; then
        echo "  ok   $name is callable from native code"
    else
        echo "  FAIL $name has no reverse P/Invoke wrapper: it would throw on a device"
        FAIL=1
    fi
done

echo
if [ "$FAIL" -ne 0 ]; then
    echo "IL2CPP CHECK FAILED. A callback is not a static [MonoPInvokeCallback]"
    echo "method; see rule 3 at the top of csharp/UniNet/Native.cs."
    exit 1
fi
echo "IL2CPP CHECK PASSED: all ${#EXPECTED[@]} callbacks converted for Android ARM64."
