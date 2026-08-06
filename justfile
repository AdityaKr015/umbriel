set positional-arguments

mode := "debug"
build-dir := "build-" + mode
prefix := "/usr/local"
cpp-std := "c++23"

default:
    @just --list

configure m=mode install_prefix=prefix:
    #!/usr/bin/env bash
    set -euo pipefail
    args=(-Dcpp_std={{cpp-std}} --prefix "{{install_prefix}}")
    case "{{m}}" in
      release)
        args+=(--buildtype=release -Db_lto=true)
        ;;
      asan)
        args+=(--buildtype=debug -Db_sanitize=address)
        ;;
      debug|*)
        args+=(--buildtype=debug)
        ;;
    esac
    if [[ -d "build-{{m}}" ]]; then
        meson setup "build-{{m}}" "${args[@]}" --reconfigure
    else
        meson setup "build-{{m}}" "${args[@]}"
    fi
    ln -sfn "build-{{m}}/compile_commands.json" compile_commands.json

_ensure-configured m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ ! -f "build-{{m}}/build.ninja" ]]; then
        just configure {{m}}
    fi

build m=mode: (_ensure-configured m)
    meson compile -C build-{{m}} umbriel

debug: (build "debug")

asan: (build "asan")

release: (build "release")

install: (build "release")
    meson install -C build-release

run m=mode: (build m)
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -z "${TERMINAL:-}" ]]; then
        echo "error: set TERMINAL to your terminal (ghostty, kitty, alacritty, ...)" >&2
        exit 1
    fi
    if [[ "{{m}}" == "asan" ]]; then
        export ASAN_OPTIONS="${ASAN_OPTIONS:-abort_on_error=1:detect_leaks=0:halt_on_error=1}"
    fi
    ./build-{{m}}/umbriel -s "$TERMINAL"

format:
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 clang-format -i
    find src tests \( -name '*.cpp' -o -name '*.h' \) -print0 | xargs -0 grep -ZlP '\s+$' | xargs -0 -r sed -i 's/[[:space:]]*$//'

_clang_tidy m=mode *args:
    #!/usr/bin/env bash
    set -euo pipefail
    src_root="$(realpath src)"
    run-clang-tidy -quiet -use-color -p "build-{{m}}" -j "$(nproc)" -header-filter='\.\./src/.*' {{args}} "^${src_root}/.*"

lint m=mode: (_ensure-configured m)
    just _clang_tidy {{m}} '-warnings-as-errors=*'

clean m=mode:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -L compile_commands.json && "$(readlink compile_commands.json)" == "build-{{m}}/compile_commands.json" ]]; then
        rm -f compile_commands.json
    fi
    rm -rf build-{{m}}

rebuild m=mode: (clean m) (build m)
