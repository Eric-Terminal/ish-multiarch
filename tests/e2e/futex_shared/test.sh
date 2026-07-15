#!/bin/sh
set -eu

gcc -std=gnu11 -Wall -Wextra -Werror -pthread test.c -o futex_shared
./futex_shared
