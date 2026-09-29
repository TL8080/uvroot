DIR = $(dir $(abspath $(firstword $(MAKEFILE_LIST))))
ROOTFS = $(DIR)/rootfs
SRC_DIR = $(DIR)/../src
UVROOT = $(SRC_DIR)/uvroot
CARE = $(SRC_DIR)/care
CC = gcc
UVROOT_RAW = $(UVROOT)

CHECK_TESTS = $(patsubst %,check-%, $(wildcard test-*.sh) $(wildcard test-*.c))

.PHONY: check clean_failure check_failure setup check-%

check: | clean_failure check_failure

memcheck: UVROOT_RAW := $(UVROOT)
memcheck: UVROOT := $(shell which valgrind) -q --error-exitcode=1 $(UVROOT)
memcheck: check

clean_failure:
	@rm -f failure

check_failure: $(CHECK_TESTS)
	@bash -c '! test -e failure'

check-%.sh: %.sh setup
	$(Q)env CARE="$(CARE)" UVROOT_RAW="$(UVROOT_RAW)" UVROOT="$(UVROOT)" ROOTFS=$(ROOTFS) PATH="$${PATH}:/bin" sh -ex $< $(silently); $(call check,$*)

check-%.c: $(ROOTFS)/bin/% setup
	$(call check_c,$*,$(UVROOT) -b /proc -r $(ROOTFS) /bin/$*)

C_SHORT_TESTS = $(patsubst %.c,check-%,$(wildcard test-*.c))
SH_SHORT_TESTS = $(patsubst %.sh,check-%,$(wildcard test-*.sh))
.PHONY: $(C_SHORT_TESTS) $(SH_SHORT_TESTS)
$(C_SHORT_TESTS): check-%: check-%.c
$(SH_SHORT_TESTS): check-%: check-%.sh



# Special cases.
check-test-sysexit.c: test-sysexit
	$(call check_c,$<,env UVROOT_FORCE_KOMPAT=1 $(UVROOT) -k 3.4242XX ./$<)

check-test-bdc90417.c: test-bdc90417
	$(call check_c,$<,$(UVROOT) -w . ./$<)

check-test-5bed7141.c: test-5bed7141
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-5bed7143.c: test-5bed7143
	$(call check_c,$<,$(UVROOT) -r $(ROOTFS) -b . ./$<)
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-16573e73.c: test-16573e73
	$(call check_c,$<,$(UVROOT) ./$<)
	$(call check_c,$<,$(UVROOT) ./$< 1)

check-test-82ba4ba1.c: test-82ba4ba1
	$(call check_c,$<,$(UVROOT) -0 ./$<)
	$(call check_c,$<,! $(UVROOT) -i 123:456 ./$<)

check-test-kkkkkkkk.c: test-kkkkkkkk
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-25069c12.c: test-25069c12
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-25069c13.c: test-25069c13
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-1ffc8309.c: test-1ffc8309
	$(call check_c,$<,env UVROOT_FORCE_KOMPAT=1 $(UVROOT) -k $(shell uname -r) ./$<)

check-test-c5a7a0f0.c: test-c5a7a0f0
	$(call check_c,$<,$(UVROOT) -0 ./$<)
	$(call check_c,$<,! $(UVROOT) -i 123:456 ./$<)

#check-test-a3e68988.c: test-a3e68988
#	@which gdb >/dev/null 2>&1 || rm -f $<
#	$(call check_c,$<,$(UVROOT) gdb -return-child-result -ex run -ex quit ./$<)

#check-test-c47aeb7d.c: test-c47aeb7d
#	@which gdb >/dev/null 2>&1 || rm -f $<
#	$(call check_c,$<,$(UVROOT) gdb -return-child-result -ex run -ex quit ./$<)

check-test-fdf487a0.c: test-fdf487a0
	$(call check_c,$<,echo test | $(UVROOT) ./$<)

check-test-iiiiiiii.c: test-iiiiiiii
	$(call check_c,$<,echo test | env UVROOT_DONT_POLLUTE_ROOTFS=1 $(UVROOT) -b /bin:/this_shall_not_exist_outside_uvroot ./$<)

check-test-9c07fad8.c: test-9c07fad8
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-fa205b56.c: test-fa205b56
	$(call check_c,$<,$(UVROOT) ./$<)

check-test-bug-182.c: test-bug-182
	$(call check_c,$<,$(UVROOT) ./$<)

check_c = $(Q)if [ -e $< ]; then			\
		$(2) $(silently); $(call check,$(1))	\
	else						\
		echo "  CHECK	$(1) skipped";		\
	fi

check = case "$$?" in					\
		0)   echo "  CHECK	$(1) ok";;	\
		125) echo "  CHECK	$(1) skipped";;	\
		*)   echo "  CHECK	$(1) FAILED";	\
		     touch failure ;;			\
	esac

######################################################################
# Build a clean rootfs

ROOTFS_BIN = $(ROOTFS)/bin/true $(ROOTFS)/bin/false    				 \
       $(ROOTFS)/bin/pwd  $(ROOTFS)/bin/readlink $(ROOTFS)/bin/symlink		 \
       $(ROOTFS)/bin/abs-true $(ROOTFS)/bin/rel-true $(ROOTFS)/bin/echo		 \
       $(ROOTFS)/bin/argv0 $(ROOTFS)/bin/readdir $(ROOTFS)/bin/cat		 \
       $(ROOTFS)/bin/chdir_getcwd $(ROOTFS)/bin/fchdir_getcwd $(ROOTFS)/bin/argv \
       $(ROOTFS)/bin/fork-wait $(ROOTFS)/bin/ptrace $(ROOTFS)/bin/ptrace-2	 \
       $(ROOTFS)/bin/ptrace-3 $(ROOTFS)/bin/gdb-ptrace-test $(ROOTFS)/bin/gdb-ptrace-test-signal	\
       $(ROOTFS)/bin/puts_proc_self_exe $(ROOTFS)/bin/exec $(ROOTFS)/bin/exec-m32 \
       $(ROOTFS)/bin/exec-suid $(ROOTFS)/bin/exec-sgid $(ROOTFS)/bin/exec-m32-suid \
       $(ROOTFS)/bin/exec-m32-sgid $(ROOTFS)/bin/getresuid $(ROOTFS)/bin/getresgid \
       $(ROOTFS)/bin/chroot

ROOTFS_DIR = $(ROOTFS)/bin $(ROOTFS)/tmp

$(ROOTFS_BIN): | $(ROOTFS_DIR)

$(ROOTFS_DIR):
	@mkdir -p $@

setup: $(ROOTFS_BIN)

$(ROOTFS)/bin/abs-true:
	$(Q)ln -fs $$(which true) $@

$(ROOTFS)/bin/rel-true:
	$(Q)ln -fs ./true $@

$(ROOTFS)/bin/exec-m32: exec.c
	$(Q)$(CC) -m32 -static $^ -o $@ $(silently) || true

$(ROOTFS)/bin/exec-suid: $(ROOTFS)/bin/exec
	$(Q)cp $^ $@
	$(Q)chmod u+s $@

$(ROOTFS)/bin/exec-sgid: $(ROOTFS)/bin/exec
	$(Q)cp $^ $@
	$(Q)chmod g+s $@

$(ROOTFS)/bin/exec-m32-suid: $(ROOTFS)/bin/exec-m32
	$(Q)cp $^     $@ $(silently) || true
	$(Q)chmod u+s $@ $(silently) || true

$(ROOTFS)/bin/exec-m32-sgid: $(ROOTFS)/bin/exec-m32
	$(Q)cp $^ $@      $(silently) || true
	$(Q)chmod g+s $@  $(silently) || true

.SECONDARY: $(patsubst %.c,$(ROOTFS)/bin/%, $(wildcard test-*.c))
$(ROOTFS)/bin/%: %.c
	$(Q)$(CC) -static $*.c -o $@ $(silently) || true

# Special cases.
test-bdc90417: test-bdc90417.c
	$(Q)$(CC) $< -o $@ $(silently) || true

# Not supported anymore.
# test-af062114: test-af062114.c
# 	$(Q)$(CC) $< -Wl,-rpath=foo -o $@ $(silently) || true

test-5bed7141: test-5bed7141.c
	$(Q)$(CC) $< -pthread -static -o $@ $(silently) || true

test-16573e73: test-16573e73.c
	$(Q)$(CC) $< -static -o $@ $(silently) || true

test-82ba4ba1: test-82ba4ba1.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-kkkkkkkk: test-kkkkkkkk.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-25069c12: test-25069c12.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-25069c13: test-25069c13.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-5bed7143: test-5bed7143.c
	$(Q)$(CC) $< -static -o $@ $(silently) || true

test-1ffc8309: test-1ffc8309.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-c5a7a0f0: test-c5a7a0f0.c
	$(Q)$(CC) $< -pthread -static -o $@ $(silently) || true

test-a3e68988: test-a3e68988.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-fdf487a0: test-fdf487a0.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-iiiiiiii: test-iiiiiiii.c
	$(Q)$(CC) $< -o $@ $(silently) || true

test-9c07fad8: test-9c07fad8.c
	$(Q)$(CC) -fPIE -pie $< -o $@ $(silently) || true

test-fa205b56: test-fa205b56.c
	$(Q)$(CC) $< -pthread -o $@ $(silently) || true

#test-bug-182: test-bug-182.c
#	$(Q)$(CC) $< -o $@ $(silently) || true

test-c47aeb7d: test-c47aeb7d.c
	$(Q)$(CC) $< -pthread -o $@ $(silently) || true

######################################################################
# Beautified output

V = 0
QUIET_LOG = /dev/null
ifeq ($(V), 0)
    quiet = quiet_
    Q     = @
    silently = >> $(QUIET_LOG) 2>&1
else
    quiet = 
    Q     = 
    silently = 
endif
