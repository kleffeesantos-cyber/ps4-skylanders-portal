# LEGO Dimensions Toy Pad - PS4 plugin
#
#   make test     compiles and runs the protocol tests on this PC (no PS4 SDK needed)
#   make sim      runs the whole plugin on this PC with a fake game (test/host_sim.c)
#   make web      regenerates source/web.h from web/index.html

CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O1 -g
SRC     := source/portal.c

# 'make' with no arguments runs the tests. 'web' is only needed when web/index.html changes
# (source/web.h is committed, so the plugin build does not need it).
.DEFAULT_GOAL := test
.PHONY: test sim web clean

web:
	python3 tools/gen_web.py

test: test_lego
	./test_lego

test_lego: test/test_portal.c $(SRC) source/portal.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -Isource -o $@ test/test_portal.c $(SRC)

sim:
	sh test/run_host_sim.sh

clean:
	rm -f test_lego
