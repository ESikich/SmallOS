#!/usr/bin/env bash
set -euo pipefail

cc=${SMALLOS_CC:-i686-elf-gcc}
crt0=${SMALLOS_CRT0:?SMALLOS_CRT0 is required}
args=("$@")
extra_libs=()
if [ -n "${SMALLOS_HOSTED_LIBS:-}" ]; then
    # shellcheck disable=SC2206
    extra_libs=(${SMALLOS_HOSTED_LIBS})
fi

inject=1
out=

for ((i = 0; i < ${#args[@]}; i++)); do
    arg=${args[$i]}
    case "$arg" in
        -c|-S|-E|-shared|-r)
            inject=0
            ;;
        -Wl,-r|-Wl,--relocatable)
            inject=0
            ;;
        -o)
            if [ $((i + 1)) -lt ${#args[@]} ]; then
                out=${args[$((i + 1))]}
            fi
            ;;
    esac
done

case "$out" in
    *.o|*.lo|*.a|*.la|*.so)
        inject=0
        ;;
esac

if [ "$inject" -eq 1 ]; then
    exec "$cc" "$crt0" -Wl,-e,_start "${args[@]}" "${extra_libs[@]}"
fi

exec "$cc" "${args[@]}"
