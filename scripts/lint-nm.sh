#!/bin/sh -e

# Find the repository root directory
REPO_ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$(dirname -- "$0")")" && pwd)"

# Read the expected version:
EXPECT_VERSION="$(awk -F'=' '/^cabal=/{print$2}' ./scripts/dev-dependencies.txt)"

# Find cabal:
#
# 1. Use CABAL if it is set.
# 2. Look for cabal-$EXPECTED_VERSION.
# 3. Look for cabal.
#
if [ "${CABAL}" = "" ]; then
	if ! CABAL="$(which "cabal-${EXPECT_VERSION}")"; then
		if ! CABAL="$(which "cabal")"; then
			echo "Requires cabal ${EXPECT_VERSION}; no version found"
			exit 1
		fi
	fi
fi

# Check cabal version:
ACTUAL_VERSION="$("${CABAL}" --numeric-version | head -n 1)"
if [ "${ACTUAL_VERSION}" != "${EXPECT_VERSION}" ]; then
	# Version mismatch is never an error:
	echo "Requires cabal ${EXPECT_VERSION}; version ${ACTUAL_VERSION} found"
fi

# Find nm:
#
# 1. Use NM if it is set.
# 2. Look for nm.
#
if [ "${NM}" = "" ]; then
	if ! NM="$(which "nm")"; then
		echo "Requires nm; no version found"
		exit 1
	fi
fi

# Configure based on platform
case "$(uname)" in
	Linux)
		SYMBOL_PREFIX=""
		DYNAMIC_LIBRARY_EXTENSION="so"
		STATIC_LIBRARY_EXTENSION="a"
		;;
	Darwin)
		SYMBOL_PREFIX="_"
		DYNAMIC_LIBRARY_EXTENSION="dylib"
		STATIC_LIBRARY_EXTENSION="a"
		;;
	*)
		echo "Unsupported platform '${OSTYPE}'"
		exit 1
		;;
esac

# Build eventlog-socket library
BUILDDIR="${REPO_ROOT_DIR}/dist-newstyle/lint-nm"
CABAL_ARGS="-f+control -f+debug"
${CABAL} clean --builddir="${BUILDDIR}" >&2
${CABAL} build eventlog-socket --builddir="${BUILDDIR}" ${CABAL_ARGS} >&2

# Find dynamic library
DYNAMIC="$(echo "${BUILDDIR}/build/"*"/ghc-"*"/eventlog-socket-"*"/build/"*".${DYNAMIC_LIBRARY_EXTENSION}")"
if [ -f "${DYNAMIC}" ]; then
	echo "Found dynamic library at:"
	echo "${DYNAMIC}"
	echo
else
	echo "Could not find dynamic library."
	exit 1
fi

# Create temporary file for bad symbols
DYNAMIC_SYMBOL_WARN_LIST="$(mktemp)"
trap 'rm -f "${DYNAMIC_SYMBOL_WARN_LIST}"' EXIT

# Lint symbols from dynamic library
${NM} -gUP "${DYNAMIC}" | while read line; do
	symbol="$(echo "${line}" | cut -d' ' -f1)"
	case "${symbol}" in
	${SYMBOL_PREFIX}EventLogSocket*)
		;;
	${SYMBOL_PREFIX}eventlog_socket_*)
		;;
	${SYMBOL_PREFIX}eventlogzmsocketzm0*)
		;;
	${SYMBOL_PREFIX}zdeventlogzmsocketzm0*)
		;;
	${SYMBOL_PREFIX}ghczuwrapperZC*ZCeventlogzmsocketzm0*)
		;;
	${SYMBOL_PREFIX}_*)
		# These should correspond to reserved symbols.
		;;
	*)
		if [ ! -z "${symbol}" ]; then
			echo "${symbol}" >>"${DYNAMIC_SYMBOL_WARN_LIST}"
		fi
		;;

	esac
done
if [ -s "${DYNAMIC_SYMBOL_WARN_LIST}" ]; then
	echo "The following symbols should NOT be exported from the dynamic library:"
	echo
	cat "${DYNAMIC_SYMBOL_WARN_LIST}"
	echo
fi

# Find static library
STATIC="$(echo "${BUILDDIR}/build/"*"/ghc-"*"/eventlog-socket-"*"/build/"*".${STATIC_LIBRARY_EXTENSION}")"
if [ -f "${STATIC}" ]; then
	echo "Found static library at:"
	echo "${STATIC}"
	echo
else
	echo "Could not find static library."
	exit 1
fi

# Create temporary file for bad symbols
STATIC_SYMBOL_WARN_LIST="$(mktemp)"
trap 'rm -f "${STATIC_SYMBOL_WARN_LIST}"' EXIT

# Lint symbols from static library
${NM} -gUP "${STATIC}" | while read line; do
	symbol="$(echo "${line}" | cut -d' ' -f1)"
	case "${symbol}" in
	${SYMBOL_PREFIX}EventLogSocket*)
		;;
	${SYMBOL_PREFIX}eventlog_socket_*)
		;;
	${SYMBOL_PREFIX}eventlogzmsocketzm0*)
		;;
	${SYMBOL_PREFIX}zdeventlogzmsocketzm0*)
		;;
	${SYMBOL_PREFIX}ghczuwrapperZC*ZCeventlogzmsocketzm0*)
		;;
	# NOTE: These are the additional clauses for the static library.
	*:)
		# Object file names.
		;;
	${SYMBOL_PREFIX}es_*)
		;;
	*)
		if [ ! -z "${symbol}" ]; then
			echo "${symbol}" >>"${STATIC_SYMBOL_WARN_LIST}"
		fi
		;;

	esac
done
if [ -s "${STATIC_SYMBOL_WARN_LIST}" ]; then
	echo "The following symbols should NOT be exported from the static library:"
	echo
	cat "${STATIC_SYMBOL_WARN_LIST}"
	echo
fi

# Print the number of warnings and exit.
if [ -s "${DYNAMIC_SYMBOL_WARN_LIST}" -o -s "${STATIC_SYMBOL_WARN_LIST}" ]; then
	WARN_COUNT="$(awk 'END {print NR}' "${DYNAMIC_SYMBOL_WARN_LIST}" "${STATIC_SYMBOL_WARN_LIST}")"
	echo "Encountered ${WARN_COUNT} warnings."
	exit 1
fi
