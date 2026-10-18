# Changelog

## Unreleased

- The 68008 core has begun as a standalone library, clocked per byte cycle. It runs NOP, MOVEQ, MOVE, MOVEA, LEA and PEA in all twelve addressing modes. It is checked against SingleStepTests/m68000, with the 68008's bus cycles and timings derived from the 68000's.
