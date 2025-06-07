# !/bin/bash

cd $1
make -f Makefile.linux-generic clean
make -f Makefile.linux-generic
gcc -g -c -O0 sqlite3.c -o sqlite3.o
gcc -g shell.c sqlite3.o -o shell.o
cp sqlite3.o ./test_mods/sqlite3