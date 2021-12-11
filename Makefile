UNAME_S = $(shell uname -s)

OBJ = main.o loadelf.o disassemble.o helper.o interpret.o saveState.o githash.o

ifeq ($(UNAME_S),Linux)
	CXX = g++-9 -march=native -flto
	EXTRA_LD = -ldl -lffi -lbfd -lunwind -lboost_program_options -lcapstone
endif

ifeq ($(UNAME_S),FreeBSD)
	CXX = CC -march=native
	EXTRA_LD = -L/usr/local/lib -lunwind -lboost_program_options -lcapstone
endif

ifeq ($(UNAME_S),Darwin)
	CXX = clang++ -march=native -I/opt/local/include
	EXTRA_LD = -L/opt/local/lib -lboost_program_options-mt -lcapstone
endif

CXXFLAGS = -std=c++11 -g $(OPT)
LIBS =  $(EXTRA_LD) -lpthread

DEP = $(OBJ:.o=.d)
OPT = -O3 -g -fomit-frame-pointer -std=c++11
EXE = interp_mips

.PHONY : all clean

all: $(EXE)

$(EXE) : $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LIBS) -o $(EXE)

githash.cc : .git/HEAD .git/index
	echo "const char *githash = \"$(shell git rev-parse HEAD)\";" > $@

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 


-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
