#!/bin/sh

find . -name "*.cc" -o -name "*.h" | xargs clang-format -i --style='{ColumnLimit: 120}'
