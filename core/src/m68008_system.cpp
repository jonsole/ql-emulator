// The 68008 core: the instructions of the supervisor state and the ones
// that raise exceptions -- TRAP, TRAPV, CHK, RTE, STOP, RESET, the USP
// moves, and the moves and logic to SR and CCR.

#include "m68008_internal.h"

namespace ql {

void M68008::trap(uint16_t opcode) {
    // TRAP #n: 0100 1110 0100 nnnn. The return address is the next
    // instruction's, two bytes on from the opcode.
    idle(4);
    exception(VECTOR_TRAP_0 + (opcode & 0xF), regs_.pc - 2);
}

void M68008::trapv() {
    // TRAPV: with V clear, a prefetch and nothing more. With V set, the same
    // read goes out but already as a supervisor program access, even from the
    // user state, and its word is thrown away by the exception that follows,
    // which returns to the next instruction (SingleStepTests).
    if (!(regs_.sr & SR_V)) {
        prefetch();
        return;
    }
    read_word(regs_.pc, FunctionCode::SupervisorProgram);
    exception(VECTOR_TRAPV, regs_.pc - 2);
}

void M68008::chk(uint16_t opcode) {
    // CHK <ea>,Dn: 0100 rrr1 10 mmm xxx, word-sized on the 68000. Traps if Dn
    // is above the bound or below zero. The manual defines N only when it
    // traps and leaves Z, V and C undefined; the 68000 sets N and Z from Dn
    // and clears V and C (SingleStepTests).
    int16_t bound = static_cast<int16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    int16_t value = static_cast<int16_t>(regs_.d[(opcode >> 9) & 7]);
    uint16_t ccr = regs_.sr & SR_X;
    if (value < 0) ccr |= SR_N;
    if (value == 0) ccr |= SR_Z;
    regs_.sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | ccr);
    if (value >= 0 && value <= bound) {
        idle(6);
        prefetch();
        return;
    }
    // Trapping takes 8 clocks or 10. Above the bound with Dn not negative it
    // is 8. With Dn negative the trap is certain, and the time follows the
    // sign of the 16-bit Dn - bound with its overflow ignored: 8 if that is
    // above zero, 10 if not. Worked out from every register-bound case in
    // SingleStepTests' CHK file; the manual gives a single time for the trap.
    int16_t difference = static_cast<int16_t>(value - bound);
    idle(value >= 0 || difference > 0 ? 8 : 10);
    exception(VECTOR_CHK, regs_.pc - 2);
}

void M68008::rte() {
    // RTE: privileged. SR, then PC's high and low words, all read from the
    // supervisor stack before the new SR takes effect; the queue is then
    // filled at the return address under the new SR's function codes
    // (SingleStepTests).
    if (!supervisor_or_trap()) return;
    uint16_t sr = read_word(regs_.ssp, FunctionCode::SupervisorData);
    uint32_t high = read_word(regs_.ssp + 2, FunctionCode::SupervisorData);
    uint32_t pc = high << 16 | read_word(regs_.ssp + 4, FunctionCode::SupervisorData);
    regs_.ssp += 6;
    regs_.sr = static_cast<uint16_t>(sr & SR_IMPLEMENTED);
    refill_queue(pc);
}

void M68008::stop() {
    // STOP #imm: privileged. SR takes the immediate, which is already in IRC,
    // and the processor stops with the queue as it is and PC past the
    // immediate, which is where an interrupt will return to.
    if (!supervisor_or_trap()) return;
    regs_.sr = static_cast<uint16_t>(regs_.irc & SR_IMPLEMENTED);
    idle(4);
    stopped_ = true;
}

void M68008::reset_instruction() {
    // RESET: privileged. The 68000 asserts its RESET output for 124 clocks to
    // reset the devices on the bus, and the CPU itself carries on (User's
    // Manual, RESET; 128 internal clocks in SingleStepTests). Nothing on the
    // QL is known to need the pulse yet, so the Bus is not told.
    if (!supervisor_or_trap()) return;
    idle(128);
    prefetch();
}

void M68008::move_usp(uint16_t opcode) {
    // MOVE An,USP (0100 1110 0110 0rrr) and MOVE USP,An (0110 1rrr).
    // Privileged. A7 here is the supervisor stack pointer.
    if (!supervisor_or_trap()) return;
    if (opcode & 8) {
        address_register(opcode & 7) = regs_.usp;
    } else {
        regs_.usp = address_register(opcode & 7);
    }
    prefetch();
}

void M68008::logic_to_status(uint16_t opcode, bool to_sr) {
    // ORI, ANDI and EORI to CCR (0000 0000/0010/1010 0011 1100) and to SR
    // (... 0111 1100), the immediate in the word that follows; to SR is
    // privileged. CCR takes only the immediate's low five bits' worth.
    if (to_sr && !supervisor_or_trap()) return;
    uint16_t immediate = take_extension();
    uint16_t sr = regs_.sr;
    switch (opcode & 0x0E00) {
    case 0x0000: sr |= immediate; break;
    case 0x0200: sr &= to_sr ? immediate : (immediate | ~SR_CCR); break;
    default: sr ^= immediate; break;
    }
    if (!to_sr) sr = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (sr & SR_CCR));
    regs_.sr = static_cast<uint16_t>(sr & SR_IMPLEMENTED);
    // After a change to SR the queue is thrown away and fetched again, under
    // the new function codes (SingleStepTests).
    idle(8);
    refill_queue(regs_.pc - 2);
}

void M68008::move_to_status(uint16_t opcode, bool to_sr) {
    // MOVE <ea>,CCR (0100 0100 11 mmm xxx) and MOVE <ea>,SR (0100 0110 11
    // ...), from a word; to SR is privileged, and is checked before the
    // operand is read.
    if (to_sr && !supervisor_or_trap()) return;
    uint16_t value = static_cast<uint16_t>(read(resolve((opcode >> 3) & 7, opcode & 7, Size::Word), Size::Word));
    if (!to_sr) value = static_cast<uint16_t>((regs_.sr & ~SR_CCR) | (value & SR_CCR));
    regs_.sr = static_cast<uint16_t>(value & SR_IMPLEMENTED);
    idle(4);
    refill_queue(regs_.pc - 2);
}

void M68008::move_from_sr(uint16_t opcode) {
    // MOVE SR,<ea>: 0100 0000 11 mmm xxx. Not privileged on the 68000 (it is
    // from the 68010 on).
    unsigned mode = (opcode >> 3) & 7;
    if (mode == 0) {
        regs_.d[opcode & 7] = (regs_.d[opcode & 7] & 0xFFFF0000) | regs_.sr;
        prefetch();
        idle(2);
        return;
    }
    // To memory the 68000 reads the destination before it writes it, and
    // throws the value away (SingleStepTests; the User's Manual warns of the
    // read).
    Operand dest = resolve(mode, opcode & 7, Size::Word);
    read(dest, Size::Word);
    prefetch();
    write(dest, Size::Word, regs_.sr);
}

} // namespace ql
