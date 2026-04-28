#!/bin/bash

if [ ! -d build ]; then
    echo "create out directory"
    mkdir build
fi

cd build
cmake ../

make -j 2

cd -
