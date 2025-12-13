    .section .text
    .globl _start

##################################################
# Normal instruction stream
##################################################

_start:
    addi t0, x0, 1        # I0
    addi t1, x0, 2        # I1

    ##################################################
    # Faulting instruction (LW causes mem exception)
    ##################################################
    li   t2, 0x10000      # invalid address (outside memory)
    lw   t3, 0(t2)        # LW → MEM EXCEPTION

    ##################################################
    # Younger instructions already in pipeline
    ##################################################
    addi t4, x0, 4        # I2
    addi t5, x0, 5        # I3
    addi t6, x0, 6        # I4

    ##################################################
    # Filler (never reached)
    ##################################################
    addi t7, x0, 7
    addi t7, x0, 8
    addi t7, x0, 9

    ##################################################
    # Exception handler at 0x8000
    ##################################################
    .org 0x8000
exception_handler:
    addi a0, x0, 42       # IX (clearly visible)
    .word 0xfeedfeed
