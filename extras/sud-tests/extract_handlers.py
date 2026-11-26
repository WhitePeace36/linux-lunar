#!/usr/bin/env python3
"""Pull the two SUD upcall entry points out of Wine's signal_x86_64.c so
test_upcall.c can run them.  Usage: extract_handlers.py signal_x86_64.c > handlers.inc"""
import re, sys

src = open(sys.argv[1]).read()
for name in ('__wine_syscall_dispatcher_upcall', '__wine_syscall_dispatcher_upcall_push'):
    m = re.search(r'__ASM_GLOBAL_FUNC\( ' + name + r',\n(.*?)\)\n', src, re.S)
    if not m:
        sys.exit(f'{name} not found in {sys.argv[1]}')
    body = (m.group(1)
            .replace('__ASM_NAME("syscall_dispatch_enabled")', '"syscall_dispatch_enabled"')
            .replace('__ASM_LOCAL_LABEL("__wine_syscall_dispatcher_prolog_end")', '"test_prolog_end"'))
    print('__asm__(".text\\n.globl %s\\n%s:\\n"\n%s);\n' % (name, name, body))
