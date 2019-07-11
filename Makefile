CCFILES=$(wildcard *.cc)
CXXFLAGS += -g -fPIC -std=c++17 -DSTATUS_VERSION=\"0.1\" -O1 -Wall -Wextra -pedantic
OBJECTS=$(patsubst %.cc, %.o, $(CCFILES))
LDFLAGS=-lpulse -lsystemd -g

status: $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -f status $(OBJECTS)
