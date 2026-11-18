#!/bin/sh
set -e

cmake -S "$(dirname "$0")" -B "$(dirname "$0")/../../build-remoting"
cmake --build "$(dirname "$0")/../../build-remoting"
"$(dirname "$0")/../../build-remoting/ticbuild_remoting_smoke" 19091 /tmp/ticbuild-remoting-sessions
