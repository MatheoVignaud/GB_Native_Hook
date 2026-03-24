#include "Decompiler.h"

bool decompile = false;
DecompilerProgram decompiler_program = {0};



void decompiler_main(char *rom, char *output_dir, char *sym, uint16_t entry_point)
{
    decompile = true;
    printf("Decompiler main called with rom=%s, output_dir=%s, sym=%s, entry_point=%04X\n", rom, output_dir, sym, entry_point);

    // open the ROM file
    FILE *f = fopen(rom, "rb");
    if (!f)
    {
        fprintf(stderr, "Failed to open ROM file: %s\n", rom);
        return;
    }

    // read the ROM data into memory
    fseek(f, 0, SEEK_END);
    long rom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *rom_data = (uint8_t *)malloc(rom_size);
    if (!rom_data)
    {
        fprintf(stderr, "Failed to allocate memory for ROM data\n");
        fclose(f);
        return;
    }

    size_t read_size = fread(rom_data, 1, rom_size, f);
    if (read_size != (size_t)rom_size)
    {
        fprintf(stderr, "Failed to read ROM data from file\n");
        free(rom_data);
        fclose(f);
        return;
    }
    fclose(f);


    MemoryState memory;
    memory_init(&memory);

    CPUState cpu = {0};
    PPUState ppu = {0};

    cpu.memory = &memory;
    memory.cpu = &cpu;
    memory.ppu = &ppu;
    memory.ppu_synced_cycles = cpu.cycle_count;
    ppu.mem       = &memory.memory;
    ppu.mem_state = &memory;

    cpu_reset(&cpu);
    ppu_reset(&ppu, memory.bios_enabled);
    cpu.PC = entry_point;

    for (int i = 0; i < 10000000000; ++i)
    {
        uint32_t cycles = cpu_execute_instruction(&cpu);
        ppu_step(&ppu, cycles);
        if (cycles == 0)
        {
            fprintf(stderr, "Execution halted at PC=%04X\n", cpu.PC);
            break;
        }

    }

}

void decompiler_discover_function(uint16_t addr)
{
    uint32_t rom_offset = 0; // TODO: calculate ROM offset from address and bank
    if (decompiler_is_rom_visited(rom_offset))
    {
        printf("Address %04X already visited, skipping\n", addr);
        return;
    }

    decompiler_set_rom_visited(rom_offset);

    bool end_of_function = false;
    while (!end_of_function)
    {
        DecompiledInstruction instr = decompiler_analyze_instruction(NULL, addr, 0);
        printf("Analyzed instruction at %04X: opcode=%02X\n", addr, instr.opcode);

        // TODO: analyze instruction to determine control flow and update addr for next instruction
        end_of_function = true;
    }

}

DecompiledInstruction decompiler_analyze_instruction(CPUState *cpu, uint16_t address, uint16_t bank)
{
    uint8_t opcode = memory_read8(cpu->memory, address);
    bool cb_prefixed = (opcode == 0xCB);
    if (cb_prefixed)
    {
        // Handle CB-prefixed instructions
        opcode = memory_read8(cpu->memory, address + 1);
        printf("CB-prefixed instruction: opcode=%02X\n", opcode);
    }
    Decompiler_Disasemble_Opcode pattern = cb_prefixed ? disassemble_cb_opcodes[opcode] : disassemble_opcodes[opcode];
    int32_t operand = 0;
    char operand_str[40] = {0};
    if (pattern.operand_type != OPERAND_NONE)
    {
        if (pattern.operand_type == OPERAND_N8 || pattern.operand_type == OPERAND_A8 || pattern.operand_type == OPERAND_E8)
        {
            operand = memory_read8(cpu->memory, address + 1);
                if (pattern.operand_type == OPERAND_E8)
                {
                    int8_t signed_disp = (int8_t)operand;
                    sprintf(operand_str, "%02Xh (PC+%d)", operand, signed_disp);
                }
                else
                {
                    sprintf(operand_str, "%02Xh", operand);
                }
        }
        else if (pattern.operand_type == OPERAND_N16 || pattern.operand_type == OPERAND_A16)
        {
            operand = memory_read16(cpu->memory, address + 1);
            sprintf(operand_str, "%04Xh", operand);
        }
    }

    DecompiledInstruction instr = {0};
    
    instr.address = address;
    instr.bank = bank;
    instr.rom_offset = 0; // TODO

    instr.opcode = opcode;
    instr.cb_prefixed = cb_prefixed;
    instr.encoded_size = pattern.length;
    instr.cycles_min = 0; // TODO
    instr.cycles_max = 0; // TODO

    strncpy(instr.mnemonic, pattern.mnemonic, sizeof(instr.mnemonic) - 1);
    strncpy(instr.operands, operand_str, sizeof(instr.operands) - 1);

    instr.dependency_mask = 0; // TODO
    instr.read_type = 0; // TODO
    instr.write_type = 0; // TODO

    instr.read_address = 0; // TODO
    instr.write_address = 0; // TODO

    instr.branch_target = 0; // TODO
    instr.branch_disp = 0; // TODO
    instr.instruction_flags = 0; // TODO
    return instr;
}
