include make.defs

.phony: all
all: multigrid3d 

multigrid2d:  multigrid2d.cpp
		$(CXX) -c $(INC) `libpng-config --cflags` $?
		$(CXX) -o $@ $@.o $(LIB) `libpng-config --ldflags` -lhwloc -lnuma

multigrid2d+minimon:  multigrid2d+minimon.cpp minimonitoring.h
		$(CXX) -c $(INC) `libpng-config --cflags` $?
		$(CXX) -o $@ $@.o $(LIB) `libpng-config --ldflags` -lhwloc -lnuma

multigrid3d: multigrid3d.cpp allreduce.h minimonitoring.h
	$(CXX) -march=native -c $(INC) $<
	$(CXX) -march=native -o $@ $@.o $(LIB) -lrt -lnuma

multigrid3d_plain.cpp: multigrid3d.cpp
	grep -v -i "minimon"   multigrid3d.cpp > multigrid3d_plain.cpp

multigrid3d_plain: multigrid3d_plain.cpp
	$(CXX) -march=native -c $(INC) $?
	$(CXX) -march=native -o $@ $@.o $(LIB) -lrt -lnuma

multigrid3d_elastic: multigrid3d_elastic.cpp minimonitoring.h
	$(CXX) -march=native -c $(INC) $?
	$(CXX) -march=native -o $@ $@.o $(LIB) -lrt -lnuma

heat_equation2d:  heat_equation2d.cpp
	$(CXX) -c $(INC) $?
	$(CXX) -o $@ $@.o $(LIB) -lhwloc -lnuma

heat_equation3d:  heat_equation3d.cpp
	$(CXX) -c $(INC) $?
	$(CXX) -o $@ $@.o $(LIB) -lhwloc -lnuma

.phony: printenv
printenv :
	@echo "CXX           = $(CXX)"
	@echo "DART_IMPL     = $(DART_IMPL)"
	@echo "DASH_ROOT     = $(DASH_ROOT)"
	@echo "INC           = $(INC)"
	@echo "LIB           = $(LIB)"

.phony: clean
clean:
	rm -f heat_equation*d multigrid multigrid*d multigrid*d+minimon multigrid3d_elastic halo_heat_eqn *.o *.gch
