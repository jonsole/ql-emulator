# Changelog

## Unreleased

- The 68008 core has begun as a standalone library, clocked per byte cycle. It runs NOP, MOVEQ, MOVE, MOVEA, LEA and PEA in all twelve addressing modes. It is checked against SingleStepTests/m68000, with the 68008's bus cycles and timings derived from the 68000's.
- The core has the supervisor state and exceptions: TRAP, TRAPV, CHK, RTE, STOP and RESET; MOVE to and from SR and USP and MOVE to CCR; ORI, ANDI and EORI to CCR and SR; privilege violations; trace; and the illegal-instruction, line A and line F exceptions for every opcode the 68000 does not define. Opcodes are decoded through a table covering the whole instruction set; those still to be implemented say so rather than running.
- The core has program flow: Bcc and BRA with byte and word displacements, BSR, DBcc, JMP, JSR, RTS, RTR, LINK and UNLK, each with the 68000's prefetch behaviour for a branch taken and not taken.
