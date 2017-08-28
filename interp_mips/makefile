OBJ = main.o loadelf.o parseMips.o helper.o profileMips.o
CXX = g++ -march=native
EXE = interp_mips
OPT = -O3
CXXFLAGS = -std=c++11 -g $(OPT) -Wextra -Wno-unused-parameter
DEP = $(OBJ:.o=.d)

.PHONY: all clean

all: $(EXE)

$(EXE) : $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -lpthread -lunwind -o $(EXE)

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 

-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
