UNAME_S = $(shell uname -s)

OBJ = main.o loadelf.o parseMips.o helper.o profileMips.o

ifeq ($(UNAME_S),Linux)
	CXX = g++ -march=native -flto
	EXTRA_LD = -ldl -lffi -lbfd -lunwind -lboost_system -lboost_program_options
endif

ifeq ($(UNAME_S),FreeBSD)
	CXX = CC -march=native
	EXTRA_LD = -L/usr/local/lib -lunwind -lboost_system -lboost_program_options
endif

ifeq ($(UNAME_S),Darwin)
	CXX = clang++ -march=native
	EXTRA_LD = -L/opt/local/lib -lboost_program_options-mt
endif

CXXFLAGS = -std=c++11 -g $(OPT) $(LLVM_CXXFLAGS)
LIBS =  $(EXTRA_LD) -lpthread

DEP = $(OBJ:.o=.d)
OPT = -O3 -g -fomit-frame-pointer -std=c++11
EXE = interp_mips

.PHONY : all clean

all: $(EXE)

$(EXE) : $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) $(LLVM_LDFLAGS) $(LIBS) -o $(EXE)

%.o: %.cc
	$(CXX) -MMD $(CXXFLAGS) -c $< 

%.o : %.s
	as $< -o $@

-include $(DEP)

clean:
	rm -rf $(EXE) $(OBJ) $(DEP)
