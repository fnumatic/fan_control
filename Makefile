# SPDX-License-Identifier: GPL-2.0
# Lets `make` work from the repo root; module/Makefile does the work.
all clean:
	$(MAKE) -C module $@

.PHONY: all clean
