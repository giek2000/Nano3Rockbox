# Host-native build for testing our original FTL (ftl-nano3g.c) and NAND
# ID decoder (nand_vendor.c) against a simulated NAND, without any ARM
# cross-compiler or real hardware. Original test infrastructure for this
# project (see test_ftl.c for details).

FTL_SRC := ../../../firmware/target/arm/s5l8702/ipodnano3g
CC ?= gcc
CFLAGS := -std=gnu99 -Wall -Wextra -g -O1 \
          -I. -I$(FTL_SRC)

# Which FTL implementation to test:
#   make                -> v1 (block-level, ftl-nano3g.c)   [default]
#   make FTL=v2         -> v2 (page-level log-structured, ftl-nano3g-v2.c)
# Both implement the same ftl-target.h API and link against the same mock;
# they are never compiled together (single-FTL binary either way).
FTL ?= v1
ifeq ($(FTL),v2)
    FTL_IMPL := $(FTL_SRC)/ftl-nano3g-v2.c
    CFLAGS   += -DFTL_NANO3G_V2
else
    FTL_IMPL := $(FTL_SRC)/ftl-nano3g.c
endif

SRCS := test_ftl.c mock_nand.c $(FTL_IMPL) $(FTL_SRC)/nand_vendor.c
OBJS := $(notdir $(SRCS:.c=.o))

VPATH := $(FTL_SRC)

.PHONY: all clean check
all: test_ftl

test_ftl: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

check: test_ftl
	./test_ftl

clean:
	rm -f *.o test_ftl
