#!/bin/bash

docker run -it --rm \
    -v $PWD:/build:rw \
    -p 10888:8080 \
    justinhop/buildbase:alpine
