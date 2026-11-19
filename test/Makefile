# Host-side tests.
#   mock_ioc_test   the M1-M4 scenarios (IO, chains, recovery, logging) against the
#                   FreeBSD-shaped core (core/mps*.c, core/compat) and the IOC model
#   mps_port_test   attach/discovery/thin CAM/reinit smoke test of the same core
#   freestanding    compile the ported C core like the kext does (-nostdinc,
#                   only stdint/stddef/stdarg/string/libkern; test/kernhdr stands in
#                   for Kernel.framework's string.h and libkern.h)
CXX ?= c++
CC ?= cc
SAN := -fsanitize=address,undefined -fno-sanitize-recover=undefined
CXXFLAGS ?= -std=gnu++20 -O1 -g -Wall -Wextra -fno-exceptions -fno-rtti $(SAN)
PORT_CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare $(SAN)


PORT_SRCS := ../core/compat/mps_compat.c ../core/compat/cam_compat.c ../core/mps.c \
	../core/mps_sas.c ../core/mps_sas_lsi.c ../core/mps_table.c ../core/mps_xnu.c
PORT_HDRS := ../core/compat/mps_compat.h ../core/compat/cam_compat.h ../core/compat/queue.h \
	../core/mpsvar.h ../core/mps_sas.h ../core/mps_table.h ../core/mps_xnu.h ../core/mps_mpi.h
PORT_OBJS := $(patsubst ../core/%.c,obj/%.o,$(PORT_SRCS))

obj/%.o: ../core/%.c $(PORT_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(PORT_CFLAGS) -I../core -c -o $@ $<

mock_ioc_test: mock_ioc_test.cpp mps_host_os.cpp mps_host_os.h mock_ioc.h $(PORT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ mock_ioc_test.cpp mps_host_os.cpp $(PORT_OBJS)

mps_port_test: mps_port_test.cpp mps_host_os.cpp mps_host_os.h mock_ioc.h $(PORT_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ mps_port_test.cpp mps_host_os.cpp $(PORT_OBJS)

FREESTANDING_INC := $(shell $(CC) -print-file-name=include)
freestanding: $(PORT_SRCS) $(PORT_HDRS)
	@for f in $(PORT_SRCS); do \
		echo "  CC (freestanding) $$f"; \
		$(CC) -std=gnu11 -nostdinc -ffreestanding -fno-builtin -DKERNEL \
		    -isystem $(FREESTANDING_INC) -Ikernhdr -I../core \
		    -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror \
		    -c -o /dev/null $$f || exit 1; \
	done

test: mock_ioc_test mps_port_test freestanding
	./mock_ioc_test
	./mps_port_test

clean:
	rm -rf mock_ioc_test mps_port_test obj

.PHONY: test clean freestanding
