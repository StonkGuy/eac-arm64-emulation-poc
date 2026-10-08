#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Generates bigcode.h: a ~55k-instruction straight-line x86-64 function (as top-level inline asm) that fexbench executes
# exactly once, so its run time is dominated by FEX's translation throughput. Deterministic (fixed seed).
import random, sys
random.seed(7)
regs = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
r32 = {"rax": "eax", "rbx": "ebx", "rcx": "ecx", "rdx": "edx", "rsi": "esi", "rdi": "edi"}
L = [".text", ".globl bigcode", "bigcode:", "push %rbx", "push %r12", "push %r13", "push %r14", "push %r15", "push %rbp"]
ops = ["add", "sub", "xor", "and", "or", "imul", "adc", "sbb"]
for _ in range(30000):
    a, b = random.sample(regs, 2)
    op = random.choice(ops)
    L.append("imul %%%s, %%%s" % (b, a) if op == "imul" else "%s %%%s, %%%s" % (op, b, a))
    k = random.random()
    if k < 0.2: L.append("lea %d(%%%s,%%%s,%d), %%%s" % (random.randint(-100, 100), a, b, random.choice([1, 2, 4, 8]), random.choice(regs)))
    elif k < 0.35: L.append("shl $%d, %%%s" % (random.randint(1, 31), a))
    elif k < 0.45: L += ["cmp %%%s, %%%s" % (a, b), "cmovl %%%s, %%%s" % (a, b)]
    elif k < 0.55: L.append("movq %%%s, -%d(%%rsp)" % (a, 8 * random.randint(1, 64)))
    elif k < 0.65: L.append("movq -%d(%%rsp), %%%s" % (8 * random.randint(1, 64), b))
    elif k < 0.75 and a in r32: L += ["movd %%%s, %%xmm%d" % (r32[a], random.randint(0, 7)), "paddd %%xmm%d, %%xmm%d" % (random.randint(0, 7), random.randint(0, 7))]
L += ["pop %rbp", "pop %r15", "pop %r14", "pop %r13", "pop %r12", "pop %rbx", "ret"]
# 4096 tiny distinct functions + a pointer table, for the indirect-call benchmark (IL2CPP-like virtual dispatch)
for i in range(4096):
    L += [".text", ".globl tf%d" % i, "tf%d:" % i, "lea %d(%%rdi), %%rax" % (i * 3 + 1), "xor $%d, %%rax" % (i * 7), "ret"]
L += [".data", ".globl tf_table", ".balign 8", "tf_table:"] + [".quad tf%d" % i for i in range(4096)] + [".text"]
with open(sys.argv[1] if len(sys.argv) > 1 else "bigcode.h", "w") as f:
    f.write("__asm__(\n" + "\n".join('"%s\\n"' % l for l in L) + ");\n")
print(len(L), "instructions")
