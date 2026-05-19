#include <cstdint>
#include <format>
#include <utility>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <optional>

// --- RegisterIndex DEFINITION ---
class RegisterIndex {
private:
    uint32_t value;

public:
    explicit constexpr RegisterIndex(const uint32_t value) : value(value) {
    }

    [[nodiscard]] constexpr uint32_t raw() const { return value; }

    constexpr bool operator==(const RegisterIndex other) const {
        return value == other.value;
    }
};

// --- Instruction DEFINITION ---
class Instruction {
private:
    uint32_t opcode;

public:
    explicit Instruction(uint32_t opcode) : opcode(opcode) {
    }

    // Return bits [31:26] of the instruction
    [[nodiscard]] constexpr uint32_t raw() const {
        return opcode;
    }

    // Return bits [31:26] of the instruction
    [[nodiscard]] constexpr uint32_t function() const {
        return opcode >> 26;
    }

    // Return register index in bits [20:16]
    [[nodiscard]] constexpr RegisterIndex t() const {
        return RegisterIndex((opcode >> 16) & 0x1f);
    }

    // Return register index in bits [25:21]
    [[nodiscard]] constexpr RegisterIndex s() const {
        return RegisterIndex((opcode >> 21) & 0x1f);
    }

    // Return register index in bits [15:11]
    [[nodiscard]] constexpr RegisterIndex d() const {
        return RegisterIndex((opcode >> 11) & 0x1f);
    }

    // Return register index in bits [5:0]
    [[nodiscard]] constexpr uint32_t subfunction() const {
        return opcode & 0x3f;
    }

    // Shift immediate values are stored in bits [10:6]
    [[nodiscard]] constexpr uint32_t shift() const {
        return (opcode >> 6) & 0x1f;
    }

    // Return immediate value in bits [16:0]
    [[nodiscard]] constexpr uint32_t imm() const {
        return opcode & 0xffff;
    }

    // Return immediate value in bits [16:0] as a sign-extended 32 bit value
    [[nodiscard]] constexpr uint32_t imm_se() const {
        const auto v = static_cast<int16_t>(opcode & 0xffff);

        return static_cast<uint32_t>(v);
    }

    [[nodiscard]] constexpr uint32_t imm_jump() const {
        return opcode & 0x3ffffff;
    }

    // Coprocessor opcode
    // This method returns the same bit range as Instruction::s
    // however it returns it as a plain u32 instead of a RegisterIndex
    // since it's not a register in this case
    [[nodiscard]] constexpr uint32_t cop_opcode() const {
        return (opcode >> 21) & 0x1f;
    }
};

// --- BIOS DEFINITION ---
class BIOS {
private:
    std::vector<uint8_t> data;

public:
    // Fixed size: 512KB
    static constexpr uint64_t BIOS_SIZE = 512 * 1024;

    [[nodiscard]] uint32_t load32(const uint32_t offset) const {
        const auto idx = static_cast<size_t>(offset);

        const auto b0 = static_cast<uint32_t>(data[idx + 0]);
        const auto b1 = static_cast<uint32_t>(data[idx + 1]);
        const auto b2 = static_cast<uint32_t>(data[idx + 2]);
        const auto b3 = static_cast<uint32_t>(data[idx + 3]);
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    static BIOS load_from_file(const std::string &path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            throw std::system_error(errno, std::generic_category(), "Failed to open BIOS file");
        }

        BIOS bios;
        bios.data.resize(BIOS_SIZE);

        file.read(reinterpret_cast<char *>(bios.data.data()), BIOS_SIZE);
        std::streamsize bytes_read = file.gcount();

        // Ensure we loaded exactly the required BIOS size
        if (bytes_read != static_cast<std::streamsize>(BIOS_SIZE)) {
            throw std::runtime_error("Invalid BIOS size");
        }

        return bios;
    }
};

class RAM {
private:
    std::vector<uint8_t> data;

public:
    RAM() {
        // Allocate exactly 2 Megabytes (2 * 1024 * 1024 bytes)
        constexpr size_t RAM_SIZE = 2 * 1024 * 1024;

        // Initialize the buffer entirely filled with the garbage byte 0xca
        data.assign(RAM_SIZE, 0xca);
    }

    // Fetch a 32-bit little-endian word from a given RAM offset
    [[nodiscard]] constexpr uint32_t load32(const uint32_t offset) const {
        const auto idx = static_cast<size_t>(offset);

        const auto b0 = static_cast<uint32_t>(data[idx + 0]);
        const auto b1 = static_cast<uint32_t>(data[idx + 1]);
        const auto b2 = static_cast<uint32_t>(data[idx + 2]);
        const auto b3 = static_cast<uint32_t>(data[idx + 3]);

        // Reconstruct the 32-bit word
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }

    // Store a 32-bit little-endian word into a given RAM offset
    void store32(const uint32_t offset, const uint32_t val) {
        const auto idx = static_cast<size_t>(offset);

        // Slice the 32-bit value down into individual bytes
        data[idx + 0] = static_cast<uint8_t>(val);
        data[idx + 1] = static_cast<uint8_t>(val >> 8);
        data[idx + 2] = static_cast<uint8_t>(val >> 16);
        data[idx + 3] = static_cast<uint8_t>(val >> 24);
    }
};

// --- map NAMESPACE ---
namespace map {
    struct Range {
        uint32_t start;
        uint32_t size;

        [[nodiscard]] constexpr bool contains(const uint32_t addr) const {
            return addr >= start && addr < (start + size);
        }
    };

    inline constexpr std::array<uint32_t, 8> REGION_MASK = {
        0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, // KUSEG: 2048MB
        0x7fffffff, // KSEG0:  512MB
        0x1fffffff, // KSEG1:  512MB
        0xffffffff, 0xffffffff, // KSEG2: 1024MB
    };

    inline uint32_t mask_region(const uint32_t address) {
        const auto index = static_cast<size_t>(address >> 29);
        return address & REGION_MASK[index];
    }

    // All memory maps are updated to use raw physical footprints
    inline constexpr Range RAM = {0x00000000, 2 * 1024 * 1024}; // 2MB RAM
    inline constexpr Range BIOS = {0x1fc00000, 512 * 1024}; // 512KB BIOS
    inline constexpr Range SYS_CONTROL = {0x1f801000, 36}; // Memory Control
    inline constexpr Range RAM_SIZE = {0x1f801060, 4}; // RAM Configuration
    inline constexpr Range SPU = {0x1f801c00, 640}; // SPU Registers
    inline constexpr Range EXPANSION_2 = {0x1f802000, 66}; // Expansion region 2

    // CACHE_CONTROL sits in KSEG2 space directly, bypasses masking targets
    inline constexpr Range CACHE_CONTROL = {0xfffe0130, 4};
}

// --- Interconnect DEFINITION ---
class Interconnect {
private:
    BIOS bios;
    RAM ram;

public:
    explicit Interconnect(BIOS bios) : bios(std::move(bios)), ram() {
    }

    [[nodiscard]] uint32_t load32(uint32_t virtual_address) const {
        if (virtual_address % 4 != 0) {
            throw std::runtime_error(std::format("Unaligned load32 address: {:08x}", virtual_address));
        }

        // Step 1: Clean address to physical mapping space
        uint32_t physical_address = map::mask_region(virtual_address);

        // Step 2: Route using clean physical contains rules
        if (map::RAM.contains(physical_address)) {
            const uint32_t offset = physical_address - map::RAM.start;
            return ram.load32(offset); // Resolves call error completely!
        }

        if (map::BIOS.contains(physical_address)) {
            const uint32_t offset = physical_address - map::BIOS.start;
            return bios.load32(offset); // Passes to internal file structure
        }

        // Catch placeholder traps safely
        if (map::SYS_CONTROL.contains(physical_address)) {
            // Return placeholder values or throw unhandled warnings
            return 0;
        }

        throw std::runtime_error(std::format(
            "Unhandled load32 read address: virtual {:08x} (physical {:08x})",
            virtual_address, physical_address
        ));
    }

    static void store8(const uint32_t virtual_address, const uint8_t val) {
        const uint32_t physical_address = map::mask_region(virtual_address);

        if (map::EXPANSION_2.contains(physical_address)) {
            std::clog << "Unhandled write to EXPANSION_2 register\n";
            return;
        }

        throw std::runtime_error(std::format(
            "Unhandled store8 read address: virtual {:08x} (physical {:08x})",
            virtual_address, physical_address
        ));
    }

    static void store16(const uint32_t virtual_address, const uint16_t val) {
        if (virtual_address % 2 != 0) {
            throw std::runtime_error(std::format("Unaligned store16 address: {:08x}", virtual_address));
        }

        const uint32_t physical_address = map::mask_region(virtual_address);

        if (map::SPU.contains(physical_address)) {
            std::clog << "Unhandled write to SPU register\n";
            return;
        }

        throw std::runtime_error(std::format(
            "Unhandled store16 read address: virtual {:08x} (physical {:08x})",
            virtual_address, physical_address
        ));
    }

    void store32(const uint32_t virtual_address, const uint32_t value) {
        if (virtual_address % 4 != 0) {
            throw std::runtime_error(std::format("Unaligned store32 address: {:08x}", virtual_address));
        }

        // Apply masking on target write operations as well
        const uint32_t physical_address = map::mask_region(virtual_address);

        if (map::RAM.contains(physical_address)) {
            const uint32_t offset = physical_address - map::RAM.start;
            ram.store32(offset, value);
            return;
        }
        if (map::SYS_CONTROL.contains(physical_address)) {
            switch (const std::optional<uint32_t> offset = physical_address - map::SYS_CONTROL.start; offset.value()) {
                case 0:
                    if (value != 0x1f000000) {
                        throw std::runtime_error(std::format("Bad expansion 1 base address: 0x{:08x}", value));
                    }
                    break;
                case 4:
                    if (value != 0x1f802000) {
                        throw std::runtime_error(std::format("Bad expansion 2 base address: 0x{:08x}", value));
                    }
                default:
                    std::clog << "Unhandled write to MEM_CONTROL register\n";
                    break;
            }
            return;
        }

        if (map::RAM_SIZE.contains(physical_address)) {
            std::clog << "Unhandled write to RAM_SIZE register\n";
            return;
        }

        if (map::CACHE_CONTROL.contains(physical_address)) {
            std::clog << "Unhandled write to CACHE_CONTROL register\n";
            return;
        }

        throw std::runtime_error(std::format(
            "Unhandled store32 write address: virtual {:08x} (physical {:08x})",
            virtual_address, physical_address
        ));
    }
};

// --- CPU DEFINITION ---
class CPU {
private:
    // The program counter register
    uint32_t pc;

    // Next instruction to be executed, used to simulate the branch delay slot
    Instruction next_instruction;

    // General purpose registers. the first must always contain 0.
    std::array<uint32_t, 32> registers{};

    // Memory interface
    Interconnect interconnect;

    // Cop0 register 12: Status Register
    uint32_t sr;

    // 2nd set of registers used to emulate the load delay slot accurately
    // They contain the output of the current instruction
    std::array<uint32_t, 32> output_registers{};

    // Load initiated by the current instructions
    struct LoadTarget {
        RegisterIndex reg;
        uint32_t value;
    };

    LoadTarget load;

    void decode_and_execute(const Instruction instruction) {
        switch (instruction.function()) {
            case 0b000000:
                op_subfunction(instruction);
                break;
            case 0b001111:
                op_lui(instruction);
                break;
            case 0b001101:
                op_ori(instruction);
                break;
            case 0b001100:
                op_andi(instruction);
                break;
            case 0b001001:
                op_addiu(instruction);
                break;
            case 0b001000:
                op_addi(instruction);
                break;
            case 0b101011:
                op_sw(instruction);
                break;
            case 0b100011:
                op_lw(instruction);
                break;
            case 0b101001:
                op_sh(instruction);
                break;
            case 0b101000:
                op_sb(instruction);
                break;
            case 0b000010:
                op_j(instruction);
                break;
            case 0b000011:
                op_jal(instruction);
                break;
            case 0b010000:
                op_cop0(instruction);
                break;
            case 0b000101:
                op_bne(instruction);
                break;
            default:
                op_unknown(instruction);
        }
    }

    void op_subfunction(const Instruction instruction) {
        switch (instruction.subfunction()) {
            case 0b000000:
                op_sll(instruction);
                break;
            case 0b100101:
                op_or(instruction);
                break;
            case 0b101011:
                op_sltu(instruction);
                break;
            case 0b100001:
                op_addu(instruction);
                break;
            case 0b001000:
                op_jr(instruction);
                break;
            default:
                op_unknown(instruction);
        }
    }

    static void op_unknown(const Instruction instruction) {
        throw std::runtime_error(std::format(
            "Unhandled instruction 0x{:08x}",
            instruction.raw() // falling back to the decoded function bits
        ));
    }

    // Load Upper Immediate
    void op_lui(const Instruction instruction) {
        const auto imm = instruction.imm();
        const auto t = instruction.t();

        // Low 16bits are set to 0
        const auto v = imm << 16;

        set_reg(t, v);
    }

    // Bitwise Or Immediate
    void op_ori(const Instruction instruction) {
        const auto imm = instruction.imm();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto v = reg(s) | imm;

        set_reg(t, v);
    }

    void op_sw(const Instruction instruction) {
        if ((sr & 0x10000) != 0) {
            std::clog << "Ignoring store while cache is isolated" << std::endl;
            return;
        }

        const auto imm = instruction.imm_se();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto addr = reg(s) + imm;
        const auto v = reg(t);

        store32(addr, v);
    }

    void op_sll(const Instruction instruction) {
        const auto imm = instruction.shift();
        const auto t = instruction.t();
        const auto d = instruction.d();

        const auto v = reg(t) << imm;

        set_reg(d, v);
    }

    void op_addiu(const Instruction instruction) {
        const auto imm = instruction.imm_se();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto v = reg(s) + imm;

        set_reg(t, v);
    }

    void op_j(const Instruction instruction) {
        const auto imm = instruction.imm_jump();

        pc = (pc & 0xf0000000) | (imm << 2);
    }

    void op_or(const Instruction instruction) {
        const auto d = instruction.d();
        const auto s = instruction.s();
        const auto t = instruction.t();

        const auto v = reg(s) | reg(t);

        set_reg(d, v);
    }

    void op_cop0(const Instruction instruction) {
        switch (instruction.cop_opcode()) {
            case 0b00100:
                op_mtc0(instruction);
                break;
            default:
                op_unknown(instruction);
        }
    }

    void op_mtc0(const Instruction instruction) {
        const auto cpu_r = instruction.t();
        const auto cop_r = instruction.d();

        const auto v = reg(cpu_r);

        switch (cop_r.raw()) {
            case 3:
            case 5:
            case 6:
            case 7:
            case 9:
            case 11:
                if (v != 0) {
                    throw std::runtime_error(std::format("Unhandled write to cop0r{:08x}", cop_r.raw()));
                }
                break;
            case 12:
                sr = v;
                break;
            case 13:
                if (v != 0) {
                    throw std::runtime_error("Unhandled write to CAUSE register");
                }
                break;
            default:
                throw std::runtime_error(std::format("Unhandled cop0 register: {:08x}", cop_r.raw()));
        }
    }

    void branch(const uint32_t offset) {
        // Offset immediate are always shifted two places to the right
        // since PC addresses have to aligned on 32bits at all time
        const auto shifted_offset = offset << 2;

        auto current_pc = pc;

        current_pc += shifted_offset;
        current_pc -= 4;

        pc = current_pc;
    }

    void op_bne(const Instruction instruction) {
        const auto imm = instruction.imm_se();
        const auto s = instruction.s();
        const auto t = instruction.t();

        if (reg(s) != reg(t)) {
            branch(imm);
        }
    }

    void op_addi(const Instruction instruction) {
        const auto imm = static_cast<int32_t>(instruction.imm_se());
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto s_val = static_cast<int32_t>(reg(s));

        // If imm > 0, s_val cannot be greater than INT32_MAX -i
        // If imm < 0, s_val cannot be less than INT32_MIN - i
        if ((imm > 0 && s_val > INT32_MAX - imm) || (imm < 0 && s_val < INT32_MIN + imm)) {
            throw std::runtime_error("ADDI overflow");
        }

        const int32_t result = s_val + imm;

        set_reg(t, static_cast<uint32_t>(result));
    }

    void op_lw(const Instruction instruction) {
        if ((sr & 0x10000) != 0) {
            std::clog << "Ignoring store while cache is isolated" << std::endl;
            return;
        }

        const auto imm = instruction.imm_se();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto addr = reg(s) + imm;
        const auto v = load32(addr);

        load = {t, v};
    }

    void op_sltu(const Instruction instruction) {
        const auto d = instruction.d();
        const auto s = instruction.s();
        const auto t = instruction.t();

        const auto v = reg(s) < reg(t);

        set_reg(d, v);
    }

    void op_addu(const Instruction instruction) {
        const auto s = instruction.s();
        const auto t = instruction.t();
        const auto d = instruction.d();

        const auto v = reg(s) + reg(t);

        set_reg(d, v);
    }

    void op_sh(const Instruction instruction) const {
        if ((sr & 0x10000) != 0) {
            std::clog << "Ignoring store while cache is isolated" << std::endl;
            return;
        }

        const auto imm = instruction.imm_se();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto addr = reg(s) + imm;
        const auto v = reg(t);

        store16(addr, static_cast<uint16_t>(v));
    }

    void op_jal(const Instruction instruction) {
        const auto ra = pc;

        set_reg(RegisterIndex(31), ra);

        op_j(instruction);
    }

    // Bitwise AND Immediate
    void op_andi(const Instruction instruction) {
        const auto imm = instruction.imm();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto v = reg(s) & imm;

        set_reg(t, v);
    }

    void op_sb(const Instruction instruction) const {
        if ((sr & 0x10000) != 0) {
            std::clog << "Ignoring store while cache is isolated" << std::endl;
            return;
        }

        const auto imm = instruction.imm_se();
        const auto t = instruction.t();
        const auto s = instruction.s();

        const auto addr = reg(s) + imm;
        const auto v = reg(t);

        store8(addr, static_cast<uint8_t>(v));
    }

    void op_jr(const Instruction instruction) {
        const auto s = instruction.s();

        pc = reg(s);
    }

    [[nodiscard]] uint32_t load32(const uint32_t address) const {
        return interconnect.load32(address);
    }

    static void store8(const uint32_t address, const uint8_t value) {
        Interconnect::store8(address, value);
    }

    static void store16(const uint32_t address, const uint16_t value) {
        Interconnect::store16(address, value);
    }

    void store32(const uint32_t address, const uint32_t value) {
        interconnect.store32(address, value);
    }

    // Immutable register red
    [[nodiscard]] constexpr uint32_t reg(const RegisterIndex index) const {
        return registers[static_cast<size_t>(index.raw())];
    }

    // Mutable register write
    void set_reg(const RegisterIndex index, const uint32_t value) {
        output_registers[static_cast<size_t>(index.raw())] = value;
        output_registers[0] = 0;
    }

public:
    explicit CPU(Interconnect interconnect) : pc(0xbfc00000), next_instruction(0x0),
                                              interconnect(std::move(interconnect)), sr(0), load{RegisterIndex(0), 0} {
        registers.fill(0xdeadbeef);
        registers[0] = 0;
        output_registers.fill(0xdeadbeef);
        output_registers[0] = 0;
    }

    void run_next_instruction() {
        const auto current_pc = pc;
        // Fetch instruction at PC
        const auto instruction = next_instruction;
        const auto raw_op = load32(current_pc);
        next_instruction = Instruction(raw_op);

        pc = current_pc + 4;

        std::clog << std::format("0x{:08x}", instruction.raw()) << std::endl;

        // Execute the pending load if any
        // otherwise it will load $zero which is a NOP
        // set_reg works only on output_registers, so this operation won't be visible by the next instruction
        set_reg(load.reg, load.value);

        load = {RegisterIndex(0), 0};

        decode_and_execute(instruction);

        registers = output_registers;
    }
};

[[noreturn]] int main() {
    const auto bios = BIOS::load_from_file("/home/beadex/Projects/scph1001.bin");
    const auto interconnect = new Interconnect(bios);
    const auto cpu = new CPU(*interconnect);

    while (true) {
        cpu->run_next_instruction();
    }
}
