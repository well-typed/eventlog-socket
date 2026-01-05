#!/bin/sh

# Find the repository root directory
REPO_ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$(dirname -- "$0")")" && pwd)"

# Find the eventlog-socket directory
EVENTLOG_SOCKET_DIR="${REPO_ROOT_DIR}/eventlog-socket"

# Find the eventlog-socket include directory
EVENTLOG_SOCKET_INCLUDE_DIR="${EVENTLOG_SOCKET_DIR}/include"

# Find ghc-pkg
if [ -z "${GHC_PKG}" ]; then
	if ! GHC_PKG="$(which ghc-pkg)"; then
		echo "Could not find ghc-pkg" >&2
		exit 1
	fi
fi

# Get GHC include directories
set -- $(ghc-pkg field rts include-dirs --simple-output)

# Write a .clangd configuration file
CLANGD_FILE="${REPO_ROOT_DIR}/.clangd"

rm -f "${CLANGD_FILE}"
touch "${CLANGD_FILE}"
echo "CompileFlags:" >>"${CLANGD_FILE}"
echo "  Add:" >>"${CLANGD_FILE}"
echo "    - '-Wall'" >>"${CLANGD_FILE}"
echo "    - '-Wextra'" >>"${CLANGD_FILE}"
echo "    - '-Wpedantic'" >>"${CLANGD_FILE}"
echo "    - '-DDEBUG=1'" >>"${CLANGD_FILE}"
echo "    - '-I${EVENTLOG_SOCKET_INCLUDE_DIR}'" >>"${CLANGD_FILE}"
for ghc_include_dir; do
	ghc_real_include_dir="$(realpath "${ghc_include_dir}")"
	echo "    - '-I${ghc_real_include_dir}'" >>"${CLANGD_FILE}"
done
