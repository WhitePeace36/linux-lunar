# Logic tests for patch 0014 (Syscall User Dispatch upcall) and the Wine
# handlers in extras/wine/, without building or booting a kernel.
#
#   make kernel KSRC=<kernel tree with patch 0014 applied>
#       compiles kernel/entry/syscall_user_dispatch.c against mocked kernel
#       APIs and checks the register contracts, every SIGSYS fallback, prctl
#       validation and the ptrace round trip (x86-64 and non-x86 builds)
#   make wine WINE=<dlls/ntdll/unix/signal_x86_64.c with the Wine patch>
#       runs both Wine entry points the way the kernel enters them and checks
#       the syscall frame, the registers and the red zone
CFLAGS = -O2 -Wall -Wextra -Wno-unused-parameter -Werror
MOCK = -I gen -I mock -include mock/mock.h

all:
	@echo "make kernel KSRC=<patched kernel tree>  and/or  make wine WINE=<patched signal_x86_64.c>"

gen: FORCE
	@test -n "$(KSRC)" || { echo "set KSRC"; exit 1; }
	@mkdir -p gen/linux
	@echo '#include "$(abspath $(KSRC))/include/uapi/linux/prctl.h"' > gen/linux/prctl.h
	@echo '#include "$(abspath $(KSRC))/include/linux/syscall_user_dispatch.h"' > gen/linux/syscall_user_dispatch.h
	@echo '#include "$(abspath $(KSRC))/include/linux/syscall_user_dispatch_types.h"' > gen/linux/syscall_user_dispatch_types.h

kernel: gen
	$(CC) $(CFLAGS) $(MOCK) -DCONFIG_X86_64=1 -c $(KSRC)/kernel/entry/syscall_user_dispatch.c -o gen/sud.o
	$(CC) $(CFLAGS) $(MOCK) -DCONFIG_X86_64=1 test_sud.c gen/sud.o -o gen/test_sud
	$(CC) $(CFLAGS) $(MOCK) -c $(KSRC)/kernel/entry/syscall_user_dispatch.c -o gen/sud_nox86.o
	$(CC) $(CFLAGS) $(MOCK) nox86.c gen/sud_nox86.o -o gen/nox86
	gen/test_sud
	gen/nox86

wine:
	@test -n "$(WINE)" || { echo "set WINE"; exit 1; }
	@mkdir -p gen
	python3 extract_handlers.py $(WINE) > gen/handlers.inc
	$(CC) -O2 -Wall -I gen test_upcall.c -o gen/test_upcall
	gen/test_upcall

clean:
	rm -rf gen

FORCE:
.PHONY: all kernel wine clean FORCE
