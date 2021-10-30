
all: fffind


fffind: main.o
	$(CXX) -o $@ $<

clean:
	rm -f fffind
	rm -f *.o
