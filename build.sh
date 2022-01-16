#!/bin/bash
if [ ! -e build/ ]; then
  mkdir build
fi

cd build
CXX=/usr/bin/g++ cmake ..
make
./simple-http-server --path=..
