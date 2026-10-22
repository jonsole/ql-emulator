// The 68008 core: MOVE, MOVEA, MOVEQ, LEA, PEA, MOVEP and MOVEM.

#include "m68008_internal.h"

namespace ql {

void M68008::move(uint16_t opcode, Size size) {
    Operand source = resolve((opcode >> 3) & 7, opcode & 7, size);
    uint32_t value = read(source, size);
    // With the source read, the fault PC catches up with the source's
    // extension words; the destination's do not move it (SingleStepTests).
    fault_pc_ = regs_.pc;
    // A long's N and Z are not all set before its write, which shows when the
    // write takes an address error (SingleStepTests). A destination that
    // needs no address calculation -- (An), (An)+, and (xxx).L after a memory
    // source, whose write follows the read at once -- is written with the
    // flags as the source left them: the low word's after a memory source
    // (the last word read), unchanged after a register or an immediate. The
    // other destinations are written with the whole long's.
    if (size == Size::Long) {
        unsigned dest_mode = (opcode >> 6) & 7;
        unsigned dest_reg = (opcode >> 9) & 7;
        bool memory_source = source.kind == Operand::Kind::Memory;
        bool written_at_once = dest_mode == 2 || dest_mode == 3 || (dest_mode == 7 && dest_reg == 1 && memory_source);
        // To (d16,An) or (d8,An,Xn) after a register or an immediate, only N
        // and Z: V and C are cleared with the write. Otherwise all four.
        bool displaced = dest_mode == 5 || dest_mode == 6;
        uint16_t vc = displaced && !memory_source ? regs_.sr & (SR_V | SR_C) : 0;
        if (!written_at_once) {
            set_move_flags(value, Size::Long);
            regs_.sr |= vc;
        } else if (memory_source) {
            set_move_flags(value & 0xFFFF, Size::Word);
        }
        move_to_destination(opcode, size, value);
        set_move_flags(value, Size::Long);
        return;
    }
    set_move_flags(value, size);
    move_to_destination(opcode, size, value);
}

void M68008::move_to_destination(uint16_t opcode, Size size, uint32_t value) {
    unsigned source_mode = (opcode >> 3) & 7;
    unsigned dest_mode = (opcode >> 6) & 7;
    unsigned dest_reg = (opcode >> 9) & 7;

    // Where the destination's write falls against the final prefetch depends
    // on its mode: the order below is SingleStepTests', which is the 68000's
    // microcode.
    switch (dest_mode) {
    case 0:
        write({Operand::Kind::DataRegister, dest_reg, 0, data_space(), 0}, size, value);
        prefetch();
        return;
    case 2:
    case 3: {
        Operand dest = resolve(dest_mode, dest_reg, size);
        // (An)+ as a destination increments after the write, whatever the
        // size, so a fault there leaves An as it was (SingleStepTests).
        if (dest_mode == 3) undo_postincrement_on_fault(dest_reg, dest.address, step_size(dest_reg, size));
        write(dest, size, value);
        prefetch();
        return;
    }
    case 4: {
        // -(An) as a destination: no internal clocks, and the prefetch comes
        // before the write.
        uint32_t& an = address_register(dest_reg);
        uint32_t address = an - step_size(dest_reg, size);
        if (size == Size::Long) {
            // A long goes out low word first, from the top of the space down.
            // An takes the new address, and IR the prefetched word, only after
            // the writes: an odd An faults with both as they were, the MOVE
            // itself in IR (SingleStepTests).
            uint16_t fetched = read_word(regs_.pc, program_space());
            regs_.pc += 2;
            write_word(address + 2, static_cast<uint16_t>(value), data_space());
            write_word(address, static_cast<uint16_t>(value >> 16), data_space());
            an = address;
            regs_.ir = regs_.irc;
            regs_.irc = fetched;
        } else {
            prefetch();
            an = address;
            write_memory(address, size, value);
        }
        return;
    }
    case 7:
        if (dest_reg == 1 && source_mode >= 2 && !(source_mode == 7 && (opcode & 7) == 4)) {
            // (xxx).L after a memory source: the write goes out with the low
            // word of the address still waiting in IRC, and only then is it
            // taken. After a register or an immediate, both words are taken
            // first.
            uint32_t high = take_extension();
            uint32_t address = high << 16 | regs_.irc;
            write_memory(address, size, value);
            take_extension();
            prefetch();
            return;
        }
        break;
    }
    uint32_t address = control_address(dest_mode, dest_reg);
    // (xxx).L taken whole after a register or an immediate moves the fault PC
    // on past it (SingleStepTests).
    if (dest_mode == 7 && dest_reg == 1) fault_pc_ += 2;
    write_memory(address, size, value);
    prefetch();
}

Size M68008::move_size(uint16_t opcode) {
    // MOVE's own size field, bits 13-12: 01 byte, 11 word, 10 long.
    switch (opcode & 0x3000) {
    case 0x1000: return Size::Byte;
    case 0x3000: return Size::Word;
    }
    return Size::Long;
}

void M68008::moveq(uint16_t opcode) {
    // MOVEQ #d8,Dn: 0111 rrr0 dddddddd, the byte sign-extended to 32 bits.
    // N and Z from the result, V and C cleared, X untouched.
    uint32_t value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(opcode & 0xFF)));
    regs_.d[(opcode >> 9) & 7] = value;
    set_move_flags(value, Size::Long);
    prefetch();
}

void M68008::movea(uint16_t opcode) {
    // MOVEA <ea>,An: a word is sign-extended to the whole register, and the
    // condition codes are left alone.
    Size size = move_size(opcode);
    uint32_t value = read(resolve((opcode >> 3) & 7, opcode & 7, size), size);
    if (size == Size::Word) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
    address_register((opcode >> 9) & 7) = value;
    prefetch();
}

void M68008::lea(uint16_t opcode) {
    // LEA <ea>,An: 0100 rrr1 11 mmm xxx.
    unsigned mode = (opcode >> 3) & 7;
    uint32_t address = control_address(mode, opcode & 7);
    // (d8,An,Xn) and (d8,PC,Xn) spend 2 more clocks before the prefetch.
    if (mode == 6 || (mode == 7 && (opcode & 7) == 3)) idle(2);
    address_register((opcode >> 9) & 7) = address;
    prefetch();
}

void M68008::pea(uint16_t opcode) {
    // PEA <ea>: 0100 1000 01 mmm xxx.
    unsigned mode = (opcode >> 3) & 7;
    uint32_t address = control_address(mode, opcode & 7);
    if (mode == 6 || (mode == 7 && (opcode & 7) == 3)) idle(2);
    // The absolute modes push before the prefetch; the others after
    // (SingleStepTests' bus order, the 68000's microcode).
    if (mode == 7 && (opcode & 7) <= 1) {
        push_long(address);
        prefetch();
    } else {
        prefetch();
        push_long(address);
    }
}

void M68008::movep(uint16_t opcode) {
    // MOVEP: 0000 ddd1 oo00 1aaa, between Dn and every other byte from
    // (d16,Ay) on -- for 8-bit peripherals on a 16-bit bus. Opmode 100 and
    // 101 read a word and a long, 110 and 111 write them, high byte first.
    // Then the prefetch (SingleStepTests).
    uint32_t address = address_register(opcode & 7)
                       + static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(take_extension())));
    unsigned bytes = (opcode & 0x0040) ? 4 : 2;
    uint32_t& dn = regs_.d[(opcode >> 9) & 7];
    if (opcode & 0x0080) {
        for (unsigned i = 0; i < bytes; i++) {
            write_byte(address + 2 * i, static_cast<uint8_t>(dn >> (8 * (bytes - 1 - i))), data_space());
        }
    } else {
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; i++) value = value << 8 | read_byte(address + 2 * i, data_space());
        dn = bytes == 4 ? value : (dn & 0xFFFF0000) | value;
    }
    prefetch();
}

uint32_t& M68008::register_by_number(unsigned n) {
    return n < 8 ? regs_.d[n] : address_register(n - 8);
}

void M68008::movem(uint16_t opcode) {
    // MOVEM: 0100 1d00 1s mmm xxx, then a word with a bit per register; d set
    // for memory to registers, s for long. The mask is taken before the
    // address's extension words.
    bool to_registers = opcode & 0x0400;
    Size size = (opcode & 0x0040) ? Size::Long : Size::Word;
    uint32_t step = static_cast<uint32_t>(size);
    uint16_t mask = take_extension();
    unsigned mode = (opcode >> 3) & 7;
    unsigned reg = opcode & 7;
    // A fault stacks the PC past the mask and the address's extension words,
    // which is the prefetch address once they are taken (SingleStepTests).
    fault_pc_ = regs_.pc;

    if (!to_registers && mode == 4) {
        // -(An): the mask runs backwards (bit 0 is A7, bit 15 D0), the
        // registers go out from A7 down to D0, a long low word first, and An
        // ends at the last address written. If An is in the list, the value
        // written is An's before the instruction (SingleStepTests).
        uint32_t& an = address_register(reg);
        uint32_t address = an;
        for (unsigned i = 0; i < 16; i++) {
            if (!(mask & (1u << i))) continue;
            uint32_t value = register_by_number(15 - i);
            address -= step;
            if (size == Size::Long) {
                write_word(address + 2, static_cast<uint16_t>(value), data_space());
                write_word(address, static_cast<uint16_t>(value >> 16), data_space());
            } else {
                write_word(address, static_cast<uint16_t>(value), data_space());
            }
        }
        an = address;
        prefetch();
        return;
    }

    if (!to_registers) {
        // A control mode, D0 up to A7.
        uint32_t address = control_address(mode, reg);
        fault_pc_ = regs_.pc;
        for (unsigned i = 0; i < 16; i++) {
            if (!(mask & (1u << i))) continue;
            write_memory(address, size, register_by_number(i));
            address += step;
        }
        prefetch();
        return;
    }

    // Memory to registers, D0 up to A7, a word sign-extended to the whole
    // register. One more word is read past the last and thrown away before
    // the prefetch. With (An)+, An ends past the last register read, whatever
    // was loaded into it (SingleStepTests).
    FunctionCode space = (mode == 7 && (reg == 2 || reg == 3)) ? program_space() : data_space();
    uint32_t address = mode == 3 ? address_register(reg) : control_address(mode, reg);
    fault_pc_ = regs_.pc;
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        uint32_t value = read_memory(address, size, space);
        if (size == Size::Word) value = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
        register_by_number(i) = value;
        address += step;
    }
    read_word(address, space);
    if (mode == 3) address_register(reg) = address;
    prefetch();
}

} // namespace ql
