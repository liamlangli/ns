#!/bin/sh
# Run inside a release builder image (scripts/release/*.Dockerfile) with the
# exported source at /work/src. Installs the release build into /work/home/.ns.
#
#   build_in_container.sh linux|windows
set -eu
target=${1:?target}
cd /work/src

case "$target" in
linux)
    # Debian's libshaderc.so is missing its glslang and SPIRV-Tools
    # dependencies; link the static libraries into gpu.so instead, which also
    # spares users a libshaderc install.
    shaderc='-Wl,--start-group -l:libshaderc_combined.a -l:libglslang.a -l:libMachineIndependent.a'
    shaderc="$shaderc -l:libGenericCodeGen.a -l:libOSDependent.a -l:libOGLCompiler.a -l:libHLSL.a"
    shaderc="$shaderc -l:libSPIRV.a -l:libSPIRV-Tools-opt.a -l:libSPIRV-Tools.a -Wl,--end-group -lstdc++ -lpthread"
    make NS_DEBUG=0 install NS_HOME=/work/home "NS_SHADERC_LIB=$shaderc"
    ;;
windows)
    make OS=Windows NS_DEBUG=0 install NS_HOME=/work/home
    # Ship the MSYS2 DLLs ns.exe and the modules load, found by walking the
    # import tables the way the loader would.
    root=/work/home/.ns
    pending=$(ls "$root"/bin/ns.exe "$root"/lib/*.dll)
    copied=""
    while [ -n "$pending" ]; do
        next=""
        for binary in $pending; do
            for dll in $(llvm-objdump -p "$binary" | sed -n 's/^ *DLL Name: //p'); do
                [ -f "/ucrt64/bin/$dll" ] || continue
                case " $copied " in *" $dll "*) continue ;; esac
                cp "/ucrt64/bin/$dll" "$root/bin/$dll"
                copied="$copied $dll"
                next="$next $root/bin/$dll"
            done
        done
        pending=$next
    done
    echo "bundled:$copied"
    # Smoke test the result the way a user would start it, without the
    # builder's /ucrt64 on the DLL search path.
    cd /tmp && rm -rf smoke && WINEPATH= wine "$root/bin/ns.exe" create smoke >/dev/null && cd smoke &&
        WINEPATH= wine "$root/bin/ns.exe" run
    ;;
*)
    echo "build_in_container: unknown target $target" >&2
    exit 2
    ;;
esac
