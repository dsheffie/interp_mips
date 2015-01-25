OBJ = main.o loadelf.o parseMips.o helper.o profileMips.o basicBlock.o simCache.o
CXX = g++
EXE = interp_mips
CXXFLAGS = -std=c++11 -g -O3
DEP = $(OBJ:.o=.d)

all: $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -lsigsegv -o $(EXE)

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 

-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
