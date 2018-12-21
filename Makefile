CCFILES=$(wildcard *.cc)
CXXFLAGS += -ggdb -fPIC -std=c++11 -DSTATUS_VERSION=\"0.1\" -O2 -Wall -Wextra
OBJECTS=$(patsubst %.cc, %.o, $(CCFILES))
LDFLAGS=-lpulse -lsystemd -g

status: $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -f status $(OBJECTS)
