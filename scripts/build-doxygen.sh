#!/bin/sh -e

# Read the expected version:
EXPECT_VERSION="$(awk -F'=' '/^doxygen=/{print$2}' ./scripts/dev-dependencies.txt)"

# Find doxygen:
#
# 1. Use DOXYGEN if it is set.
# 2. Look for doxygen-$EXPECTED_VERSION.
# 3. Look for doxygen.
#
if [ "${DOXYGEN}" = "" ]; then
  if ! DOXYGEN="$(which "doxygen-${EXPECT_VERSION}")"; then
    if ! DOXYGEN="$(which "doxygen")"; then
      echo "Requires doxygen ${EXPECT_VERSION}; no version found"
      echo "To install, run:"
      echo
      echo "  cabal install doxygen-${EXPECT_VERSION}"
      echo
      exit 1
    fi
  fi
fi

# Check doxygen version:
ACTUAL_VERSION="$("${DOXYGEN}" --version | head -n1 | cut -d'.' -f1)"
if [ "${ACTUAL_VERSION}" != "${EXPECT_VERSION}" ]; then
  echo "Requires doxygen ${EXPECT_VERSION}; version ${ACTUAL_VERSION} found"
  # Version mismatch is an error on CI:
  [ "${CI}" = "" ] || exit 1
fi

# Build documentation for C files with doxygen
echo "Build documentation for C files using doxygen version ${ACTUAL_VERSION}"
if [ "${MODE}" == "dev" ]; then
	doxygen Doxyfile.dev
else
	doxygen Doxyfile
fi
