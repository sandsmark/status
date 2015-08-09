CCFILES=$(wildcard *.cc)
CXXFLAGS += -g -std=c++11 -DSTATUS_VERSION=\"0.1\" -O0
OBJECTS=$(patsubst %.cc, %.o, $(CCFILES))
LDFLAGS=-lpulse 

status: $(OBJECTS)
	$(CXX) -o $@ $^ $(LDFLAGS)

clean:
	rm -f status $(OBJECTS)
