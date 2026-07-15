#!/bin/sh
set -eu

gcc -std=gnu11 -Wall -Wextra -Werror -pthread test.c -o robust_lifecycle
./robust_lifecycle
