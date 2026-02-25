#!/usr/bin/env bash

set -euo pipefail

DIR="$(dirname $(readlink -f $0))"
cd "$DIR/../"

# default values, can be overwritten by cmdline args
buildDir="build"
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
  numThreads=$(nproc --all)
elif [[ "$OSTYPE" == "darwin"* ]]; then
  numThreads=$(sysctl -n hw.ncpu)
else
  cat << EOF
WARNING: Unsupported OSTYPE: cannot determine number of host CPUs"
  Defaulting to 2 threads. Use --threads N to use N threads"
EOF
  numThreads=2
fi
cmakeOptions=""
cleanBefore=no
depsPrefixesFile=""
keepLog=no
compiler=gcc
portable=no
portableInstallPrefix=""

_help() {
    cat <<EOF
usage: $0 [OPTIONS]

OPTIONS:
  -cmake='-<key>=<value> [-<key>=<value> ...]'  User defined cmake options
                                                  Note: use single quote after
                                                  -cmake= and double quotes if
                                                  <key> has multiple <values>
                                                  e.g.: -cmake='-DFLAGS="-a -b"'
  -compiler=COMPILER_NAME                       Compiler name: gcc or clang
                                                  Default: gcc
  -no-warnings
                                                Compiler warnings are
                                                considered errors, i.e.,
                                                use -Werror flag during build.
  -dir=PATH                                     Path to store build files.
                                                  Default: ./build
  -coverage                                     Enable cmake coverage options
  -clean                                        Remove build dir before compile
  -no-gui                                       Disable GUI support
  -build-man                                    Build Man Pages (optional)
  -threads=NUM_THREADS                          Number of threads to use during
                                                  compile. Default: \`nproc\` on linux
                                                  or \`sysctl -n hw.logicalcpu\` on macOS
  -keep-log                                     Keep a compile log in build dir
  -help                                         Shows this message
  -gpu                                          Enable GPU to accelerate the process
  -deps-prefixes-file=FILE                      File with CMake packages roots,
                                                  its content extends -cmake argument.
                                                  By default, "openroad_deps_prefixes.txt"
                                                  file from OpenROAD's "etc" directory
                                                  or from system "/etc".
  -portable                                     Build a redistributable package for
                                                  Ubuntu 22.04:
                                                  - disables GUI/Python/TclX
                                                  - enables OPENROAD_PORTABLE
                                                  - installs to ./dist/openroad-ubuntu22-portable
                                                  - collects shared libs into lib/
                                                  - creates a .tar.gz bundle
  -portable-prefix=PATH                         Install prefix used with -portable
                                                  (default: ./dist/openroad-ubuntu22-portable)

EOF
    exit "${1:-1}"
}

__logging()
{
        local log_file="${buildDir}/openroad_build.log"
        echo "[INFO] Saving logs to ${log_file}"
        echo "[INFO] $__CMD"
        exec > >(tee -i "${log_file}")
        exec 2>&1
}

_is_glibc_core_dep()
{
        local dep_base
        dep_base="$(basename "$1")"
        case "${dep_base}" in
            linux-vdso.so.*|ld-linux*.so.*|libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libnsl.so.*|libutil.so.*|libanl.so.*)
                return 0
                ;;
            *)
                return 1
                ;;
        esac
}

_copy_portable_dep()
{
        local dep_path="$1"
        local output_lib_dir="$2"
        local dep_name

        dep_name="$(basename "${dep_path}")"
        if _is_glibc_core_dep "${dep_path}"; then
            return
        fi
        if [[ -e "${output_lib_dir}/${dep_name}" ]]; then
            return
        fi
        cp -L "${dep_path}" "${output_lib_dir}/${dep_name}"
}

_copy_tcl_runtime()
{
        local install_prefix="$1"
        local tcl_runtime_src=""
        local tcl_runtime_dst="${install_prefix}/lib/tcl8.6"
        local candidate

        for candidate in \
            "/usr/share/tcltk/tcl8.6" \
            "/usr/share/tcl8.6" \
            "/usr/lib/tcl8.6" \
            "/usr/lib64/tcl8.6"
        do
            if [[ -f "${candidate}/init.tcl" ]]; then
                tcl_runtime_src="${candidate}"
                break
            fi
        done

        if [[ -z "${tcl_runtime_src}" ]]; then
            echo "[ERROR] Could not find Tcl runtime directory containing init.tcl" >&2
            exit 1
        fi

        echo "[INFO] Bundling Tcl runtime from ${tcl_runtime_src}"
        rm -rf "${tcl_runtime_dst}"
        mkdir -p "${tcl_runtime_dst}"
        cp -a "${tcl_runtime_src}/." "${tcl_runtime_dst}/"
}

_create_portable_launcher()
{
        local install_prefix="$1"
        local tool_name="$2"
        local tool_bin_dir="${install_prefix}/bin"
        local tool_path="${tool_bin_dir}/${tool_name}"
        local tool_real_path="${tool_bin_dir}/${tool_name}.bin"

        if [[ ! -x "${tool_path}" ]]; then
            echo "[WARN] ${tool_path} is missing, skip launcher wrapping" >&2
            return
        fi

        mv -f "${tool_path}" "${tool_real_path}"
        cat > "${tool_path}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
SELF_DIR="\$(CDPATH= cd -- "\$(dirname -- "\$0")" && pwd)"
export TCL_LIBRARY="\${SELF_DIR}/../lib/tcl8.6"
exec "\${SELF_DIR}/${tool_name}.bin" "\$@"
EOF
        chmod +x "${tool_path}"
}

_collect_portable_deps()
{
        local binary_path="$1"
        local output_lib_dir="$2"
        local search_path_string="${3:-}"
        local line
        local dep
        local soname
        local resolved
        local pass
        local copied_count
        local unresolved_count
        local -a search_paths=()
        local -a deps_lines=()
        local ld_library_path
        local re_resolved='=>[[:space:]]+(/[^[:space:]]+)'
        local re_absolute='^[[:space:]]*(/[^[:space:]]+)'
        local re_missing='^[[:space:]]*([^[:space:]]+)[[:space:]]+=>[[:space:]]+not[[:space:]]+found'

        mkdir -p "${output_lib_dir}"
        if [[ -n "${search_path_string}" ]]; then
            IFS=':' read -r -a search_paths <<< "${search_path_string}"
        fi

        echo "[INFO] Collecting runtime libraries into ${output_lib_dir}"
        unresolved_count=0
        for pass in 1 2 3; do
            copied_count=0
            unresolved_count=0

            ld_library_path="${output_lib_dir}"
            if [[ -n "${search_path_string}" ]]; then
                ld_library_path="${ld_library_path}:${search_path_string}"
            fi
            mapfile -t deps_lines < <(LD_LIBRARY_PATH="${ld_library_path}" ldd "${binary_path}" || true)

            for line in "${deps_lines[@]}"; do
                if [[ "${line}" =~ ${re_resolved} ]]; then
                    dep="${BASH_REMATCH[1]}"
                    if [[ ! -e "${output_lib_dir}/$(basename "${dep}")" ]]; then
                        _copy_portable_dep "${dep}" "${output_lib_dir}"
                        copied_count=$((copied_count + 1))
                    fi
                    continue
                fi

                if [[ "${line}" =~ ${re_absolute} ]]; then
                    dep="${BASH_REMATCH[1]}"
                    if [[ ! -e "${output_lib_dir}/$(basename "${dep}")" ]]; then
                        _copy_portable_dep "${dep}" "${output_lib_dir}"
                        copied_count=$((copied_count + 1))
                    fi
                    continue
                fi

                if [[ "${line}" =~ ${re_missing} ]]; then
                    soname="${BASH_REMATCH[1]}"
                    resolved=""

                    for dep in "${search_paths[@]}"; do
                        if [[ -e "${dep}/${soname}" ]]; then
                            resolved="${dep}/${soname}"
                            break
                        fi
                    done

                    if [[ -n "${resolved}" ]]; then
                        if [[ ! -e "${output_lib_dir}/${soname}" ]]; then
                            _copy_portable_dep "${resolved}" "${output_lib_dir}"
                            copied_count=$((copied_count + 1))
                        fi
                    else
                        echo "[WARN] Unable to resolve dependency ${soname}" >&2
                        unresolved_count=$((unresolved_count + 1))
                    fi
                fi
            done

            if [[ "${copied_count}" -eq 0 && "${unresolved_count}" -eq 0 ]]; then
                break
            fi
        done

        if [[ "${unresolved_count}" -ne 0 ]]; then
            echo "[ERROR] Portable dependency collection has unresolved libraries." >&2
            ld_library_path="${output_lib_dir}"
            if [[ -n "${search_path_string}" ]]; then
                ld_library_path="${ld_library_path}:${search_path_string}"
            fi
            LD_LIBRARY_PATH="${ld_library_path}" ldd "${binary_path}" || true
            exit 1
        fi
}

_finalize_portable_package()
{
        local build_dir="$1"
        local install_prefix
        local binary_path
        local tarball
        local cache_line
        local root_path
        local lib_search_paths_string
        local -a lib_search_paths=("/opt/or-tools/lib" "/opt/or-tools/lib64" "/usr/local/lib" "/usr/local/lib64")
        local -a unique_lib_search_paths=()
        declare -A seen_lib_search_paths

        install_prefix="$(awk -F= '/^CMAKE_INSTALL_PREFIX:PATH=/{print $2}' "${build_dir}/CMakeCache.txt" | tail -n 1)"
        if [[ -z "${install_prefix}" ]]; then
            echo "[ERROR] Could not determine CMAKE_INSTALL_PREFIX from ${build_dir}/CMakeCache.txt" >&2
            exit 1
        fi

        echo "[INFO] Installing OpenROAD into ${install_prefix}"
        cmake --install "${build_dir}"

        binary_path="${install_prefix}/bin/openroad"
        if [[ ! -x "${binary_path}" ]]; then
            echo "[ERROR] Portable binary not found at ${binary_path}" >&2
            exit 1
        fi

        # Remove old collected shared libraries from previous packaging runs.
        find "${install_prefix}/lib" -maxdepth 1 \( -type f -o -type l \) -name "*.so*" -delete

        while IFS= read -r cache_line; do
            if [[ "${cache_line}" =~ ^[A-Za-z0-9_]+_ROOT:[^=]*=(.+)$ ]]; then
                root_path="${BASH_REMATCH[1]}"
                if [[ -d "${root_path}" ]]; then
                    lib_search_paths+=("${root_path}")
                fi
                if [[ -d "${root_path}/lib" ]]; then
                    lib_search_paths+=("${root_path}/lib")
                fi
                if [[ -d "${root_path}/lib64" ]]; then
                    lib_search_paths+=("${root_path}/lib64")
                fi
            fi
        done < "${build_dir}/CMakeCache.txt"

        for dep in "${lib_search_paths[@]}"; do
            if [[ -d "${dep}" && -z "${seen_lib_search_paths[${dep}]+x}" ]]; then
                unique_lib_search_paths+=("${dep}")
                seen_lib_search_paths["${dep}"]=1
            fi
        done
        lib_search_paths_string="$(IFS=:; echo "${unique_lib_search_paths[*]}")"

        _collect_portable_deps "${binary_path}" "${install_prefix}/lib" "${lib_search_paths_string}"
        _copy_tcl_runtime "${install_prefix}"
        _create_portable_launcher "${install_prefix}" "openroad"
        _create_portable_launcher "${install_prefix}" "sta"

        tarball="${install_prefix}.tar.gz"
        tar -C "$(dirname "${install_prefix}")" -czf "${tarball}" "$(basename "${install_prefix}")"
        echo "[INFO] Portable package created: ${tarball}"
}

__CMD="$0 $@"
while [ "$#" -gt 0 ]; do
    case "${1}" in
        -h|-help)
            _help 0
            ;;
        -no-gui)
            cmakeOptions+=" -DBUILD_GUI=OFF"
            ;;
        -build-man)
            cmakeOptions+=" -DBUILD_MAN=ON"
            ;;
        -compiler=*)
            compiler="${1#*=}"
            ;;
        -no-warnings )
            cmakeOptions+=" -DALLOW_WARNINGS=OFF"
            ;;
        -coverage )
            cmakeOptions+=" -DCMAKE_BUILD_TYPE=Debug"
            cmakeOptions+=" -DCMAKE_CXX_FLAGS='-fprofile-arcs -ftest-coverage'"
            cmakeOptions+=" -DCMAKE_EXE_LINKER_FLAGS=-lgcov"
            ;;
        -cmake=*)
            cmakeOptions+=" ${1#*=}"
            ;;
        -clean )
            cleanBefore=yes
            ;;
        -dir=* )
            buildDir="${1#*=}"
            ;;
        -keep-log )
            keepLog=yes
            ;;
        -threads=* )
            numThreads="${1#*=}"
            ;;
        -deps-prefixes-file=*)
            file="${1#-deps-prefixes-file=}"
            if [[ ! -f "$file" ]]; then 
                echo "${file} does not exist" >&2
                _help
            fi
            depsPrefixesFile="$file"
            ;;
        -portable)
            portable=yes
            ;;
        -portable-prefix=*)
            portable=yes
            portableInstallPrefix="${1#*=}"
            ;;
        -compiler | -cmake | -dir | -threads | -install | -deps-prefixes-file | -portable-prefix )
            echo "${1} requires an argument" >&2
            _help
            ;;
        -gpu)
            cmakeOptions+=" -DGPU=ON"
            ;;
        *)
            echo "unknown option: ${1}" >&2
            _help
            ;;
    esac
    shift 1
done

if [[ "${portable}" == "yes" ]]; then
    cmakeOptions+=" -DOPENROAD_PORTABLE=ON"
    cmakeOptions+=" -DBUILD_GUI=OFF -DBUILD_PYTHON=OFF -DBUILD_TCLX=OFF"
    cmakeOptions+=" -DENABLE_TESTS=OFF"
    cmakeOptions+=" -DABC_SKIP_TESTS=ON"
    cmakeOptions+=" -DCMAKE_BUILD_TYPE=Release"

    if [[ -n "${portableInstallPrefix}" ]]; then
        cmakeOptions+=" -DCMAKE_INSTALL_PREFIX=${portableInstallPrefix}"
    elif [[ ! "${cmakeOptions}" =~ CMAKE_INSTALL_PREFIX ]]; then
        cmakeOptions+=" -DCMAKE_INSTALL_PREFIX=$(pwd)/dist/openroad-ubuntu22-portable"
    fi
fi

if [[ -z "$depsPrefixesFile" ]]; then
    if [[ -f "$DIR/openroad_deps_prefixes.txt" ]]; then
        depsPrefixesFile="$DIR/openroad_deps_prefixes.txt"
    elif [[ -f "/etc/openroad_deps_prefixes.txt" ]]; then
        depsPrefixesFile="/etc/openroad_deps_prefixes.txt"
    fi
fi
if [[ -f "$depsPrefixesFile" ]]; then
    cmakeOptions+=" $(cat "$depsPrefixesFile")"
    echo "[INFO] Using additional CMake parameters from $depsPrefixesFile"
else
    echo "[INFO] Auto-generated prefix file does not exist - CMake will choose the dependencies automatically"
fi

case "${compiler}" in
    "gcc" )
        if [[ -f "/opt/rh/devtoolset-8/enable" ]]; then
            # the scl script has unbound variables
            set +u
            source /opt/rh/devtoolset-8/enable
            set -u
        fi
        export CC="$(command -v gcc)"
        export CXX="$(command -v g++)"
        ;;
    "clang" )
        if [[ -f "/opt/rh/llvm-toolset-7.0/enable" ]]; then
            # the scl script has unbound variables
            set +u
            source /opt/rh/llvm-toolset-7.0/enable
            set -u
        fi
        export CC="$(command -v clang)"
        export CXX="$(command -v clang++)"
        ;;
    "clang-16" )
        export CC="$(command -v clang-16)"
        export CXX="$(command -v clang++-16)"
        ;;
    *)
        export CC=""
        export CXX=""
esac

if [[ -z "${CC}" || -z "${CXX}" ]]; then
        echo "Compiler $compiler not installed or it is not supported." >&2
        _help 1
fi

if [[ "${cleanBefore}" == "yes" ]]; then
    rm -rf "${buildDir}"
fi

mkdir -p "${buildDir}"
__logging

if [[ "$OSTYPE" == "darwin"* ]]; then
    export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
    export CMAKE_PREFIX_PATH=$(brew --prefix or-tools)
fi

echo "[INFO] Using ${numThreads} threads."
eval cmake "${cmakeOptions}" -B "${buildDir}" .
eval time cmake --build "${buildDir}" -j "${numThreads}"

if [[ "${portable}" == "yes" ]]; then
    _finalize_portable_package "${buildDir}"
fi
