#!/usr/bin/env bash

set -eufo pipefail
# We want to see what's going on
set -x

# Now run the tests. The engflow build uses pinned browsers
# so this should be fine
# shellcheck disable=SC2046
bazel test --config=remote-ci --build_tests_only \
  --test_tag_filters=-exclusive-if-local,-skip-remote \
  --keep_going --flaky_test_attempts=2 \
  //dotnet/...  \
  //java/... \
  //javascript/atoms/... \
  //javascript/node/selenium-webdriver/... \
  //javascript/webdriver/... \
  //py/... \
  //rb/spec/... -- $(cat .skipped-tests | tr '\n' ' ')

# Build the packages we want to ship to users
bazel build --config=remote-ci \
  //dotnet:all \
  //java/src/... \
  //javascript/node/selenium-webdriver:selenium-webdriver \
  //py:selenium-wheel \
  //rb:selenium-devtools //rb:selenium-webdriver
