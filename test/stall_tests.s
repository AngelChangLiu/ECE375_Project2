    .section .text
    .globl _start

_start:
    ##################################################
    # Setup memory
    ##################################################
    li   t0, 256          # base address
    li   t1, 10
    sw   t1, 0(t0)
    li   t1, 20
    sw   t1, 4(t0)

    ##################################################
    # 1️⃣ LOAD → USE STALL
    # lw followed immediately by dependent add
    ##################################################
    lw   t2, 0(t0)        # load value
    add  t3, t2, t2      # 🔴 load-use stall here

    ##################################################
    # 2️⃣ LOAD → BRANCH STALL
    ##################################################
    lw   t4, 4(t0)        # load value
    beq  t4, t3, skip1   # 🔴 load-branch stall

    addi t5, x0, 1        # should be squashed if branch taken

skip1:
    ##################################################
    # 3️⃣ ALU → BRANCH STALL
    ##################################################
    add  t6, t3, t4
    beq  t6, t6, skip2   # 🔴 ALU-branch stall

    addi t5, x0, 2        # squashed

skip2:
    ##################################################
    # 4️⃣ BACK-TO-BACK LOADS (no stall, cache behavior)
    ##################################################
    lw   s0, 0(t0)
    lw   s1, 4(t0)

    ##################################################
    # 5️⃣ STORE with forwarded data
    ##################################################
    add  s2, s0, s1
    sw   s2, 8(t0)        # should forward from EX/MEM

    ##################################################
    # 6️⃣ NO STALL BASELINE
    ##################################################
    addi s3, x0, 5
    addi s4, x0, 6
    add  s5, s3, s4       # no stall

    ##################################################
    # HALT
    ##################################################
    .word 0xfeedfeed
