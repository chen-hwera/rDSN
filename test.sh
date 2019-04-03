#!/bin/bash

./run.sh format
./run.sh start_zk
ulimit -c unlimited -S
this_path=`readlink -f $0`
export DSN_ROOT=`dirname $this_path`
./run.sh build --build_plugins
./run.sh install
./run.sh test
