############################################################
# ALL PIPELINE & CACHE TESTS
# Fully assembler-safe version (NO li/NO la for labels)
############################################################

############################################################
# SECTION 1: SANITY / BASIC FLOW
############################################################

_start:
    nop

############################################################
# SECTION 2: ARITHMETIC FORWARDING (EX → EX)
############################################################

arith_forward:
    li t1, 5
    add t2, t1, t1
    add t3, t2, t1

############################################################
# SECTION 3: LOAD-USE HAZARD
############################################################

load_use:
    auipc t0, %pcrel_hi(data1)
    addi  t0, t0, %pcrel_lo(data1)
    lw    t1, 0(t0)
    add   t2, t1, t1          # load-use → stall

############################################################
# SECTION 4: MEM → EX FORWARDING
############################################################

mem_forward:
    auipc t0, %pcrel_hi(data2)
    addi  t0, t0, %pcrel_lo(data2)
    lw    t3, 0(t0)
    nop
    add   t4, t3, t3

############################################################
# SECTION 5: STORE → LOAD SAME ADDRESS
############################################################

store_load:
    auipc t0, %pcrel_hi(data3)
    addi  t0, t0, %pcrel_lo(data3)
    li    t5, 99
    sw    t5, 0(t0)
    lw    t6, 0(t0)

############################################################
# SECTION 6: BRANCH NOT TAKEN
############################################################

branch_not_taken:
    li t1, 1
    li t2, 2
    beq t1, t2, bnt_target
    addi t3, zero, 7
bnt_target:
    nop

############################################################
# SECTION 7: BRANCH TAKEN (SQUASH)
############################################################

branch_taken:
    li t4, 3
    li t5, 3
    beq t4, t5, bt_target
    addi t6, zero, 999        # must be squashed
bt_target:
    addi t6, zero, 5

############################################################
# SECTION 8: I-CACHE PRESSURE
############################################################

icache_test:
    addi t0, zero, 0
    addi t0, t0, 1
    addi t0, t0, 1
    addi t0, t0, 1
    addi t0, t0, 1
    addi t0, t0, 1
    addi t0, t0, 1
    addi t0, t0, 1

############################################################
# SECTION 9: D-CACHE MISS + STALL
############################################################

dcache_miss:
    auipc t0, %pcrel_hi(data4)
    addi  t0, t0, %pcrel_lo(data4)
    lw    t1, 0(t0)
    addi  t2, t1, 1

############################################################
# SECTION 10: LRU EVICTION
############################################################

lru_test:
    auipc t0, %pcrel_hi(data5)
    addi  t0, t0, %pcrel_lo(data5)
    lw    t1, 0(t0)
    lw    t2, 16(t0)
    lw    t3, 32(t0)
    lw    t4, 48(t0)
    lw    t1, 0(t0)            # refresh MRU
    lw    t5, 64(t0)           # evict LRU

############################################################
# HALT
############################################################

.word 0xfeedfeed

############################################################
# DATA
############################################################

.data
.align 4

data1: .word 21
data2: .word 7
data3: .word 0
data4: .word 10
data5:
    .word 1
    .word 2
    .word 3
    .word 4
    .word 5
    .word 6
