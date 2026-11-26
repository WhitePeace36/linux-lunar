#!/usr/bin/env python3
"""Pull Wine's syscall dispatcher assembly out of signal_x86_64.c for the tests.

  extract_handlers.py signal_x86_64.c handlers  > handlers.inc    (the two SUD upcall entries)
  extract_handlers.py signal_x86_64.c dispatcher > dispatcher.inc (dispatcher + upcall entries)
"""
import re, sys

src = open(sys.argv[1]).read()
what = sys.argv[2] if len(sys.argv) > 2 else 'handlers'
if what == 'dispatcher':
    start = src.index('__ASM_GLOBAL_FUNC( __wine_syscall_dispatcher,\n')
    end = src.index('/***********************************************************************\n *           __wine_unix_call_dispatcher\n')
    print(src[start:end])
    sys.exit(0)
for name in ('__wine_syscall_dispatcher_upcall', '__wine_syscall_dispatcher_upcall_push'):
    m = re.search(r'__ASM_GLOBAL_FUNC\( ' + name + r',\n(.*?)\)\n', src, re.S)
    if not m:
        sys.exit(f'{name} not found in {sys.argv[1]}')
    body = re.sub(r'__ASM_NAME\("([^"]+)"\)', r'"\1"', m.group(1))
    body = body.replace('__ASM_LOCAL_LABEL("__wine_syscall_dispatcher_prolog_end")', '"test_prolog_end"')
    print('__asm__(".text\\n.globl %s\\n%s:\\n"\n%s);\n' % (name, name, body))
