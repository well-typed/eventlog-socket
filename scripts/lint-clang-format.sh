#!/bin/sh -e

# Read the expected version:
EXPECT_VERSION="$(awk -F'=' '/^clang-format=/{print$2}' ./scripts/dev-dependencies.txt)"

# Find clang-format:
#
# 1. Use CLANG_FORMAT if it is set.
# 2. Look for clang-format-$EXPECTED_VERSION.
# 3. Look for clang-format.
#
if [ "${CLANG_FORMAT}" = "" ]; then
  if ! CLANG_FORMAT="$(which "clang-format-${EXPECT_VERSION}")"; then
    if ! CLANG_FORMAT="$(which "clang-format")"; then
      echo "Requires clang-format ${EXPECT_VERSION}; no version found"
      echo "To install, run:"
      echo
      echo "  cabal install clang-format-${EXPECT_VERSION}"
      echo
      exit 1
    fi
  fi
fi

# Check clang-format version:
ACTUAL_VERSION="$("${CLANG_FORMAT}" --version | head -n1 | cut -d' ' -f3 | cut -d'.' -f1)"
if [ "${ACTUAL_VERSION}" != "${EXPECT_VERSION}" ]; then
  echo "Requires clang-format ${EXPECT_VERSION}; version ${ACTUAL_VERSION} found"
  # Version mismatch is an error on CI:
  [ "${CI}" = "" ] || exit 1
fi

# Format C files
echo "Format C files with clang-format version ${ACTUAL_VERSION}"
# shellcheck disable=SC2086
git ls-files --exclude-standard --no-deleted --deduplicate '*.c' '*.h' | xargs -L50 ${CLANG_FORMAT} --dry-run -Werror
