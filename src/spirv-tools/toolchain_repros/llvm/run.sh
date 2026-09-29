#!/bin/sh
# Compile each LLVM reproducer with llc for SPIR-V and, if that succeeds,
# validate the result for Vulkan 1.3 with spirv-val. Library-style
# reproducers (no entry point) are validated with the universal SPIR-V 1.6
# rules, which allow their Linkage capability.
#
#   LLC=/path/to/llc SPIRV_VAL=/path/to/spirv-val ./run.sh *.ll
LLC=${LLC:-llc}
VAL=${SPIRV_VAL:-spirv-val}
for f in "$@"; do
    if grep -q '"hlsl.shader"' "$f"; then env=vulkan1.3; else env=spv1.6; fi
    if ! out=$("$LLC" -O2 -filetype=obj "$f" -o /tmp/repro_out.spv 2>&1); then
        why=$(printf '%s\n' "$out" | grep -E "LLVM ERROR|Running pass" | tail -1 | sed 's/^ *//')
        echo "$f: llc FAILED: ${why:-crashed}"
    elif ! v=$("$VAL" --target-env $env /tmp/repro_out.spv 2>&1); then
        echo "$f: spirv-val FAILED: $(printf '%s\n' "$v" | head -1)"
    else
        echo "$f: ok"
    fi
done
