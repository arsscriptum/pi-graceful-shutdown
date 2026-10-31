#!/bin/bash

PATCH_PATH=$(pwd)

pushd "$PATCH_PATH" >/dev/null
pushd .. >/dev/null

ROOT_PATH=$(pwd)

if[[ -d "$PATCH_PATH" ]]; then
  cp ./patch/iboot-agent-relay-state-fix.patch "$ROOT_PATH" -v
  git apply iboot-agent-relay-state-fix.patch
  make && make test
fi