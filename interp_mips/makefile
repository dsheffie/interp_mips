OBJ = main.o loadelf.o parseMips.o helper.o profileMips.o
CXX = CC -march=native
EXE = interp_mips
OPT = -O3
CXXFLAGS = -std=c++11 -g $(OPT) -Wextra -Wno-unused-parameter -I/usr/local/include
DEP = $(OBJ:.o=.d)

.PHONY: all clean

all: $(EXE)

$(EXE) : $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -lpthread -L/usr/local/lib/ -lunwind -o $(EXE)

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 

-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
