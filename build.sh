#!/bin/bash
cd build
CXX=/usr/bin/g++ cmake ..
make
./simple-http-server --path=..
