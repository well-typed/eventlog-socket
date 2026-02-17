#!/bin/sh -e

# Get the project root directory:
ROOT_DIR="$(CDPATH= cd -- "$(dirname "$(dirname "$(dirname -- "$0")")")" && pwd)"

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

# Build Doxygen for C API
DOXYGEN_DIR="${ROOT_DIR}/dist-newstyle/doxygen/html"
[ -d "${DOXYGEN_DIR}" ] && rm -r "${DOXYGEN_DIR}"
MODE=prod ${ROOT_DIR}/scripts/build-doxygen.sh

# Build Haddock for Haskell API.
${CABAL} haddock eventlog-socket --haddock-for-hackage

# Find Haddock archive.
HADDOCK_TAR_GZ="$(echo "${ROOT_DIR}/dist-newstyle/eventlog-socket-"*"-docs.tar.gz")"

# Extract Haddock archive.
DISTDIR="${HADDOCK_TAR_GZ%.tar.gz}"
DIST="$(basename "${DISTDIR}")"
WORKDIR="$(dirname "${DISTDIR}")"
[ -d "${DISTDIR}" ] && rm -r "${DISTDIR}"
tar -xzvf "${HADDOCK_TAR_GZ}" -C "${WORKDIR}"
rm "${HADDOCK_TAR_GZ}"

# Merge Doxygen into Haddock.
cp -r "${DOXYGEN_DIR}" "${DISTDIR}/c"

# Rebuild Haddock archive.
tar -czvf "${HADDOCK_TAR_GZ}" --format="ustar" -C "${WORKDIR}" "${DIST}"

# Clean up DISTDIR.
[ -d "${DISTDIR}" ] && rm -r "${DISTDIR}"
