# SPDX-License-Identifier: GPL-2.0
# Targets for the repo root; plain "make" lists them. module/Makefile builds the module.

B := tests/build
KDIR ?= /lib/modules/$(shell uname -r)/build
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all
# Every <linux/...> header that module/minisforum_ec.h includes, as a stub of tests/kernel.h
STUBS := $(addprefix $(B)/,$(shell grep -o '<linux/[^>]*>' module/minisforum_ec.h | tr -d '<>'))

help: ## list these targets
	@grep -hE '^[a-z]+:.*## ' $(MAKEFILE_LIST) | awk -F ':.*## ' '{ printf "make %-7s %s\n", $$1, $$2 }'

all: ## build the module (module/minisforum_ec.ko)
	$(MAKE) -C module $@

test: $(B)/fan_test ## logic tests of the fan control against a fake EC; CI runs them too
	$(B)/fan_test

$(B)/fan_test: tests/fan_test.c tests/kernel.h module/fan.c module/minisforum_ec.h $(STUBS)
	$(CC) $(CFLAGS) -I$(B) -Itests -o $@ tests/fan_test.c

$(STUBS):
	mkdir -p $(dir $@)
	echo '#include "kernel.h"' > $@

hwtest: ## load a fresh build on this board, check it, put the installed module back (sudo)
	$(MAKE) -C $(KDIR) M=$(CURDIR)/module MO=$(CURDIR)/$(B)/module modules
	sudo tests/hwtest.sh $(B)/module/minisforum_ec.ko

clean: ## remove the build output
	$(MAKE) -C module $@
	rm -rf $(B)

.PHONY: help all test hwtest clean
