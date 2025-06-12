TESTFILES := $(wildcard *.sql)
TESTS := $(TESTFILES:.sql=.test)

.PHONY: all clean

all : $(TESTS)

%.test:
	rm -f $*.c
	rm -f $*_mod.o
	rm -f $*_base.o
	python3 -m sqlite_code_gen -i $*.sql -o $*.c -f
	gcc -g $(sql_mod) $*.c -o $*_mod.o
	gcc -g $(sql_base) $*.c -o $*_base.o
	./$*_mod.o > $*_mod.out.txt
	./$*_base.o > $*_base.out.txt
	diff -B --ignore-all-space $*_mod.out.txt $*_base.out.txt && echo "All tests passed"

clean:
	rm -f *.c *.o *.c *.txt *.db *.db-shm *.db-wal
	