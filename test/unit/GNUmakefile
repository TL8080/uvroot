DIR      = $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
SRC_DIR  = $(DIR)/../../src

PKG_CONFIG = pkg-config

CPPFLAGS = -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I$(SRC_DIR)
CFLAGS   = -g -Wall -Wextra -O0 -ffunction-sections -fdata-sections
CFLAGS  += $(shell $(PKG_CONFIG) --cflags talloc check)
LDFLAGS  = -Wl,--gc-sections
LDLIBS   = $(shell $(PKG_CONFIG) --libs talloc check)

# The functions under test live in src/, compiled here rather than
# pulled from src/*.o: this only needs to build the handful of
# translation units the tests actually touch, not the whole tree.
SRCS      = test_path.c $(SRC_DIR)/path/path.c
SRCS_TEMP = test_temp.c $(SRC_DIR)/path/temp.c
OBJS      = $(notdir $(SRCS:.c=.o))
OBJS_TEMP = $(notdir $(SRCS_TEMP:.c=.o))

.PHONY: check clean

check: test_path test_temp
	./test_path
	./test_temp

test_path: $(OBJS) note.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) note.o $(LDLIBS)

test_temp: $(OBJS_TEMP) note.o
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS_TEMP) note.o $(LDLIBS)

test_path.o: test_path.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test_temp.o: test_temp.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# path.c includes build.h, which src/GNUmakefile generates.
$(SRC_DIR)/build.h:
	$(MAKE) -C $(SRC_DIR) build.h

path.o: $(SRC_DIR)/path/path.c $(SRC_DIR)/build.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

temp.o: $(SRC_DIR)/path/temp.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

note.o: $(SRC_DIR)/cli/note.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f test_path test_temp *.o *.gcda *.gcno
