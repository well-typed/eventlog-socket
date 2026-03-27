# Overview

## Scripts

The `scripts/` directory contains scripts that automate various tasks and tests.
These scripts are named after the task they perform, e.g., `build`, `lint`, `format`, and (optionally) the primary system dependency used to perform it, e.g., `clang-format`, `fourmolu`, `nm`, etc.
Every test that runs on CI corresponds to one script, so that CI failures are easy to reproduce locally.
The most important scripts are as follows:

-  <a name="scripts-pre-commit"></a>`./scripts/pre-commit.sh`

    This script runs various linting and formatting scripts and should be run prior to every commit.
    It is recommended that you install this script as a Git pre-commit hook, e.g., by running:

    ```sh
    ln ./scripts/pre-commit.sh .git/hooks/pre-commit
    ```

-   <a name="scripts-test"></a>`./scripts/test.sh`

    This script runs all tests for `eventlog-socket` and `eventlog-socket-control`.

    The script runs the test suite and passes any parameters before `--` on to Cabal and after `--` to the Tasty test runner.
    For instance, to run only `test_fibber_Unix`, run:

    ```sh
    ./scripts/test.sh -- -p /test_fibber_Unix/
    ```

    By default, the tests are linked against a version of `eventlog-socket` compiled with `-f+debug`.
    You can control the debug verbosity by passing one of the `-f+debug-verbosity-X` flags.
    You can control whether or not all received events are logged using the `-f+debug-events` flag.
    For instance, to run the test suite with `TRACE` verbosity and events, run:

    ```sh
    ./scripts/test.sh -f+debug-verbosity-trace -f+debug-events --
    ```

    By default, the script prints the test suite overview to stdout and writes a detailed test log to `eventlog-socket-tests.err.log`.
    If the `DEBUG` environment variable is defined, the script prints the detailed test log, and writes the test suite overview *and* the detailed test log to `eventlog-socket-tests.out.log` and `eventlog-socket-tests.err.log`, respectively.

    The Tasty suite runs tests concurrently, which means the detailed test log contains the interleaved output of multiple tests.
    It can be helpful to run the test suite with `-j1` to ensure that all tests are run sequentially:

    ```sh
    ./scripts/test.sh -- -j1
    ```

-   <a name="scripts-lint-nm"></a>`./scripts/lint-nm.sh`

    This script builds the `eventlog-socket` library and checks that all exposed symbols follow [the naming convention](#naming-convention-for-c-functions).

-   <a name="scripts-setup-clangd"></a>`./scripts/setup-clangd.sh`

    This script generates a `.clangd` file in the repository root.

-   <a name="scripts-build-haddock"></a>`./scripts/build-haddock.sh`

    This script builds the Haddock documentation for Hackage. It does the following:

    - It runs `cabal haddock --haddock-for-hackage`.
    - It runs `./scripts/build-doxygen.sh`.
    - It adds the built Doxygen documentation into the Hackage archive.

    When developing, use `cabal haddock` directly, e.g.,

    ```sh
    cabal haddock eventlog-socket --open
    ```

-   <a name="scripts-build-doxygen"></a>`./scripts/build-doxygen.sh`

    This script builds the *public* Doxygen documentation, i.e. only `eventlog_socket.h`, to `dist-newstyle/doxygen/html/`.

    When developing, set `MDOE=dev` to build the *full* Doxygen documentation to `dist-newstyle/doxygen-dev/html/`:

    ```sh
    MODE=dev ./scripts/build-doxygen.sh
    ```

    To view the Doxygen documentation, use your favourite HTTP server. For example:

    ```sh
    # For the public Doxygen documentation
    python -m http.server -d dist-newstyle/doxygen/html/

    # For the full Doxygen documentation
    python -m http.server -d dist-newstyle/doxygen-dev/html/
    ```

-   <a name="scripts-ttyd"></a>`./scripts/ttyd.sh`

    This script repeatedly runs a command until it fails.
    It is useful for debugging race conditions.

    ```sh
    ./scripts/ttyd.sh './scripts/test.sh -- -p /test_oddball_Reconnect_Unix/'
    ```

    > [!IMPORTANT]
    > The command must be passed in quotes.

    > [!NOTE]
    > The name stands for "try 'til you die".

## Naming Convention for C Functions

All functions and global constants in the `eventlog-socket` C library must follow these rules:

- All public functions, i.e., those declared in `eventlog_socket.h`, must be prefixed with `eventlog_socket_`.

- All hidden functions, i.e., those declared in other header files, must be prefixed with `es_${HEADER_NAME}_`, e.g., `es_control`, and must be declared `HIDDEN`.

- All other functions must be declared `static`.

- All public constants, i.e., those declared in `eventlog_socket.h`, must be prefixed with `EventLogSocket`.

- There may not be any hidden constants.

- All other constants must be declared `static`.

## Publishing a release for `eventlog-socket`

The process for making a well-documented release of `eventlog-socket` is a bit tedious, due to the presence of the Doxygen documentation and the differences in Hackage and GitHub Markdown.

1.  Ensure that the current HEAD is ready to be published:

    - The version number is updated in all relevant places.
      This includes at least `eventlog-socket/eventlog-socket.cabal`, `eventlog-socket/CHANGELOG.md`, `Doxygen`, and `Doxygen.dev`.
      This includes the `source-repository this` declaration in `eventlog-socket/eventlog-socket.cabal`, which should refer to the Git tag that you will create in step (2).
    - Every public declaration has documentation with a `@since` annotation.
      This includes all declarations exposed from `Socket.hsc` and `eventlog_socket.h`.
    - The Haddock documentation builds without warnings and renders without errors.
    - The Doxygen documentation builds without warnings and renders without errors.
    - The tests pass on CI.

2.  Create a Git tag of the form `eventlog-socket-${VERSION}`, e.g., `eventlog-socket-0.1.2.0`:
    ```sh
    git tag eventlog-socket-${VERSION}
    ```

    > [!CAUTION]
    > Replace `${VERSION}` with the new version.

3.  Publish the Git tag:
    ```sh
    git push --tags
    ```

4.  Edit the `README.md` file to make the following changes:

    - Replace the unsupported `[!NOTE]` element with the ⚠️ emoji. For example:

      ```diff
      - [!NOTE]
      + ⚠️
      ```

    - Replace relative links into the GitHub repository with permalinks into the tag. For example:

      ```diff
      - [`eventlog-socket-tests`](eventlog-socket-tests/)
      + [`eventlog-socket-tests`](https://github.com/well-typed/eventlog-socket/tree/eventlog-socket-${VERSION}/eventlog-socket-tests/)
      ```

      > [!CAUTION]
      > Replace `${VERSION}` with the new version.

    > [!CAUTION]
    > Do not commit these changes.

5.  Build the source distribution.

    ```sh
    cabal sdist eventlog-socket
    ```

    > [!TIP]
    > This writes the source distribution to `dist-newstyle/sdist/eventlog-socket-${VERSION}.tar.gz`.

6.  Upload the source distribution to Hackage *as a package candidate*

    - Navigate to <https://hackage.haskell.org/packages/candidates/upload>.
    - Upload the source distribution built in the previous step.

7.  Ensure that the package candidate page has no errors.

8.  Ensure that the `CHANGELOG.md` has no errors.

9.  Build the Haddock documentation. (See [./scripts/build-haddock.sh](#scripts-build-haddock).)

    ```sh
    ./scripts/build-haddock.sh
    ```

    > [!TIP]
    > This writes the documentation to `dist-newstyle/eventlog-socket-${VERSION}-docs.tar.gz`.

10. Upload the Haddock documentation *to the package candidate page*.

    - On the package candidate package, click on *"edit package information"*.
    - Under *"Manage Documentation"*, click on the candidate version *"eventlog-socket-${VERSION}"*.

      > [!CAUTION]
      > Replace `${VERSION}` with the new version.

    - Upload the documentation built in the previous step.

11. Ensure that the documentation for the Haskell modules has no errors.

    > [!WARNING]
    > Since package candidates are published at different URLs, the link to the Doxygen documentation is broken for package candidates.

12. Publish the candidate package.

    - On the package candidate package, click on *"[Publish]"* and confirm.

13. Revert the changes to `README.md`.

## Publishing a release for `eventlog-socket-control`


1.  Ensure that the current HEAD is ready to be published:

    - The version number is updated in all relevant places. This includes at least `eventlog-socket-control/eventlog-socket-control.cabal` and `eventlog-socket-control/CHANGELOG.md`. This includes the `source-repository this` declaration in `eventlog-socket-control/eventlog-socket-control.cabal`, which should refer to the Git tag that you will create in step (2).
    - Every public declaration has documentation with a `@since` annotation.
    - The Haddock documentation builds without warnings and renders without errors.
    - The tests pass on CI.

2.  Create a Git tag of the form `eventlog-socket-control-${VERSION}`, e.g., `eventlog-socket-control-0.1.1.0`:
    ```sh
    git tag eventlog-socket-control-${VERSION}
    ```

    > [!CAUTION]
    > Replace `${VERSION}` with the new version.

3.  Publish the Git tag:
    ```sh
    git push --tags
    ```

4. Build the source distribution.

    ```sh
    cabal sdist eventlog-socket-control
    ```

    > [!TIP]
    > This writes the source distribution to `dist-newstyle/sdist/eventlog-socket-control-${VERSION}.tar.gz`.

6.  Upload the source distribution to Hackage *as a package candidate*

    - Navigate to <https://hackage.haskell.org/packages/candidates/upload>.
    - Upload the source distribution built in the previous step.

7.  Ensure that the package candidate page has no errors.

8.  Ensure that the `CHANGELOG.md` has no errors.

9.  Publish the candidate package.

    - On the package candidate package, click on *"[Publish]"* and confirm.
