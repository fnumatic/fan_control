# SPDX-License-Identifier: GPL-2.0
# Lets `make` work from the repo root; module/Makefile does the work.
# make test    logic tests of module/fan.c against a fake EC; no hardware (CI runs them too)
# make hwtest  build the module, load it on a supported board and check it (asks for sudo)

B := tests/build
KDIR ?= /lib/modules/$(shell uname -r)/build
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all
# Every <linux/...> header that module/minisforum_ec.h includes, as a stub of tests/kernel.h
STUBS := $(addprefix $(B)/,$(shell grep -o '<linux/[^>]*>' module/minisforum_ec.h | tr -d '<>'))

all:
	$(MAKE) -C module $@

test: $(B)/fan_test
	$(B)/fan_test

$(B)/fan_test: tests/fan_test.c tests/kernel.h module/fan.c module/minisforum_ec.h $(STUBS)
	$(CC) $(CFLAGS) -I$(B) -Itests -o $@ tests/fan_test.c

$(STUBS):
	mkdir -p $(dir $@)
	echo '#include "kernel.h"' > $@

hwtest:
	$(MAKE) -C $(KDIR) M=$(CURDIR)/module MO=$(CURDIR)/$(B)/module modules
	sudo tests/hwtest.sh $(B)/module/minisforum_ec.ko

clean:
	$(MAKE) -C module $@
	rm -rf $(B)

.PHONY: all test hwtest clean
