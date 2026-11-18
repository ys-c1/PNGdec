SRC=../../src
CFLAGS=-D__LINUX__ -O2 -Wall -I$(SRC)
COBJS=adler32.o crc32.o infback.o inffast.o inflate.o inftrees.o zutil.o

all: run

ring_test: ring_test.cpp $(COBJS) PNGdec.o
	$(CXX) $(CFLAGS) ring_test.cpp PNGdec.o $(COBJS) -o ring_test

PNGdec.o: $(SRC)/PNGdec.cpp $(SRC)/png.inl $(SRC)/PNGdec.h
	$(CXX) $(CFLAGS) -c $(SRC)/PNGdec.cpp -o PNGdec.o

%.o: $(SRC)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

fixtures: gen_fixtures.py ref_decode.py
	python3 -I gen_fixtures.py fixtures
	python3 -I ref_decode.py fixtures

run: ring_test fixtures
	./ring_test fixtures

clean:
	rm -rf *.o ring_test fixtures

.PHONY: all run clean fixtures
