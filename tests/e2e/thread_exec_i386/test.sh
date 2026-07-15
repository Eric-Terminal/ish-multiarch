#!/bin/sh
set -eu

gcc -std=gnu11 -Wall -Wextra -Werror -pthread test.c -o thread_exec_i386
./thread_exec_i386
