# Changelog

## Unreleased

- The 68008 core has begun as a standalone library, clocked per byte cycle. It runs NOP, MOVEQ, MOVE, MOVEA, LEA and PEA in all twelve addressing modes. It is checked against SingleStepTests/m68000, with the 68008's bus cycles and timings derived from the 68000's.
- The core has the supervisor state and exceptions: TRAP, TRAPV, CHK, RTE, STOP and RESET; MOVE to and from SR and USP and MOVE to CCR; ORI, ANDI and EORI to CCR and SR; privilege violations; trace; and the illegal-instruction, line A and line F exceptions for every opcode the 68000 does not define. Opcodes are decoded through a table covering the whole instruction set; those still to be implemented say so rather than running.
- The core has program flow: Bcc and BRA with byte and word displacements, BSR, DBcc, JMP, JSR, RTS, RTR, LINK and UNLK, each with the 68000's prefetch behaviour for a branch taken and not taken.
- The core has integer arithmetic: ADD, SUB, ADDA, SUBA, ADDQ, SUBQ, ADDI, SUBI, ADDX, SUBX, NEG, NEGX, CMP, CMPA, CMPI, CMPM, CLR, TST, EXT, SWAP and EXG, with the 68000's flags, including X and the extended instructions only ever clearing Z.
- The core has logic and bit operations: AND, OR, EOR and NOT with ANDI, ORI and EORI; BTST, BCHG, BCLR and BSET in both forms, the bit numbered modulo 32 on a register and modulo 8 in memory; Scc; and TAS.
- The core has the shifts and rotates: ASL, ASR, LSL, LSR, ROL, ROR, ROXL and ROXR, on a data register by an immediate count or a register's modulo 64, and on a word in memory by one bit, each taking the 68000's time for its count.
- The core has MOVEM, in word and long sizes and every allowed addressing mode, the predecrement form with its reversed register order, and MOVEP.
- The core has multiply, divide and BCD: MULU, MULS, DIVU and DIVS, each taking the 68000's operand-dependent time, with divide by zero and overflow; and ABCD, SBCD and NBCD with the flags the manual leaves undefined set as the 68000 sets them. With these the core runs the whole 68000 instruction set.
- The core takes interrupts: the machine sets the IPL level, levels 1-6 are taken above the mask in SR and level 7 on each rise to it, with an autovectored acknowledge cycle the Bus sees, and an interrupt above its new mask wakes the processor from STOP.
- The core takes address errors: a word or long access at an odd address, an odd PC included, abandons the instruction where the 68000 does and stacks the seven-word group 0 frame with the 68000's stacked PC, status word and register state. The core now passes all 317,500 SingleStepTests/m68000 tests. An address error while stacking that frame, where the 68000 halts, is reported as an error rather than modelled.
