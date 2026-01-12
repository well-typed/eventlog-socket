#!/bin/sh

cabal clean && cabal test eventlog-socket-tests -f+debug "$@" 2>eventlog-socket-tests.log
