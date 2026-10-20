// The 68008 core: program flow -- Bcc, BRA, BSR, DBcc, JMP, JSR, RTS, RTR,
// LINK and UNLK.

#include "m68008_internal.h"

namespace ql {

uint32_t M68008::branch_target(uint16_t opcode) const {
    // Bcc, BRA and BSR: 0110 cccc dddddddd. A displacement byte of zero
    // means a word displacement follows, already in IRC. Either is relative
    // to the address after the opcode, 2 behind PC.
    int32_t displacement = static_cast<int8_t>(opcode & 0xFF);
    if (displacement == 0) displacement = static_cast<int16_t>(regs_.irc);
    return regs_.pc - 2 + static_cast<uint32_t>(displacement);
}

void M68008::bcc(uint16_t opcode) {
    // Taken: 2 internal clocks, and the queue filled afresh at the target.
    // Not taken: 4, and on past the displacement word if there is one
    // (SingleStepTests).
    if (condition((opcode >> 8) & 0xF)) {
        uint32_t target = branch_target(opcode);
        idle(2);
        refill_queue(target);
        return;
    }
    idle(4);
    if ((opcode & 0xFF) == 0) take_extension();
    prefetch();
}

void M68008::bsr(uint16_t opcode) {
    // BSR: 2 internal clocks, the return address pushed high word first --
    // past the displacement word if there is one -- and the queue filled at
    // the target (SingleStepTests).
    uint32_t target = branch_target(opcode);
    uint32_t return_address = (opcode & 0xFF) == 0 ? regs_.pc : regs_.pc - 2;
    idle(2);
    push_long(return_address);
    refill_queue(target);
}

void M68008::dbcc(uint16_t opcode) {
    // DBcc Dn,d16: 0101 cccc 1100 1rrr. With the condition true, 4 internal
    // clocks and on past the displacement word. Otherwise the low word of Dn
    // counts down, and unless it has run out to -1 the branch is taken: 2
    // internal clocks and the queue filled at the target (SingleStepTests).
    if (condition((opcode >> 8) & 0xF)) {
        idle(4);
        take_extension();
        prefetch();
        return;
    }
    uint32_t& dn = regs_.d[opcode & 7];
    uint16_t count = static_cast<uint16_t>(dn - 1);
    dn = (dn & 0xFFFF0000) | count;
    uint32_t target = regs_.pc - 2 + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(regs_.irc)));
    idle(2);
    if (count != 0xFFFF) {
        refill_queue(target);
        return;
    }
    // Run out: the manual gives 14(3/0) on the 68000, so three words are
    // read. The first is taken to be at the branch target, fetched before the
    // count is seen to have run out and thrown away; then the queue is filled
    // past the displacement word. SingleStepTests has no case to confirm the
    // order (m68008_flow_tests checks the time and the result).
    read_word(target, program_space());
    refill_queue(regs_.pc);
}

uint32_t M68008::jump_target(unsigned mode, unsigned reg, uint32_t& next) {
    // The extension words are used where they sit: the first is in IRC, and
    // (xxx).L's second is read from PC without moving it. The internal clocks
    // are SingleStepTests': 2 for a displacement or a short address, 6 with
    // an index, none for (An) or a long address.
    uint16_t extension = regs_.irc;
    int32_t displacement = static_cast<int16_t>(extension);
    next = regs_.pc;
    switch (mode) {
    case 2:
        next = regs_.pc - 2;
        return address_register(reg);
    case 5:
        idle(2);
        return address_register(reg) + static_cast<uint32_t>(displacement);
    case 6:
        idle(6);
        return indexed(address_register(reg), extension);
    }
    switch (reg) {
    case 0:
        idle(2);
        return static_cast<uint32_t>(displacement);
    case 1: {
        uint32_t low = read_word(regs_.pc, program_space());
        next = regs_.pc + 2;
        return static_cast<uint32_t>(extension) << 16 | low;
    }
    case 2:
        idle(2);
        return regs_.pc - 2 + static_cast<uint32_t>(displacement);
    default:
        idle(6);
        return indexed(regs_.pc - 2, extension);
    }
}

void M68008::jmp(uint16_t opcode) {
    // JMP <ea>: 0100 1110 11 mmm xxx, control modes.
    uint32_t next;
    uint32_t target = jump_target((opcode >> 3) & 7, opcode & 7, next);
    refill_queue(target);
}

void M68008::jsr(uint16_t opcode) {
    // JSR <ea>: 0100 1110 10 mmm xxx, control modes. The return address is
    // pushed, high word first, between the two reads that fill the queue at
    // the target (SingleStepTests).
    uint32_t next;
    uint32_t target = jump_target((opcode >> 3) & 7, opcode & 7, next);
    regs_.ir = read_word(target, program_space());
    push_long(next);
    regs_.irc = read_word(target + 2, program_space());
    regs_.pc = target + 4;
}

void M68008::rts() {
    // RTS: the return address popped, and the queue filled there.
    refill_queue(pop_long());
}

void M68008::rtr() {
    // RTR: CCR popped from a word -- the system byte is left alone -- then
    // the return address, and the queue filled there.
    uint32_t& sp = address_register(7);
    uint16_t ccr = read_word(sp, data_space());
    sp += 2;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (ccr & SR_CCR));
    refill_queue(pop_long());
}

void M68008::link(uint16_t opcode) {
    // LINK An,#d16: 0100 1110 0101 0rrr. An is pushed, takes the stack
    // pointer, and the displacement is added to the stack pointer. The
    // displacement is taken first, then the push, then the prefetch
    // (SingleStepTests).
    unsigned n = opcode & 7;
    int32_t displacement = static_cast<int16_t>(take_extension());
    push_long(address_register(n));
    uint32_t& sp = address_register(7);
    address_register(n) = sp;
    sp += static_cast<uint32_t>(displacement);
    prefetch();
}

void M68008::unlk(uint16_t opcode) {
    // UNLK An: 0100 1110 0101 1rrr. The stack pointer takes An, and An is
    // popped.
    unsigned n = opcode & 7;
    address_register(7) = address_register(n);
    uint32_t value = pop_long();
    address_register(n) = value;
    prefetch();
}

} // namespace ql
