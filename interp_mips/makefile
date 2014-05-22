OBJ = main.o loadelf.o parseMips.o helper.o emulateMips.o
CXX = g++ 
all: $(OBJ)
	$(CXX) -g -O3 $(OBJ) -lmhash -o sim-mips

%.o: %.cc
	$(CXX) -g -O3 -c $< 

clean:
	rm -rf sim-mips $(OBJ)
