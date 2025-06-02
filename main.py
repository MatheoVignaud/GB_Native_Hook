import os

# cree un fichier txt avec le nom "opcodes.txt"
with open("opcodes.txt", "w") as f:
    for i in range(256):
        f.write(f"void cpu_op_0x{i:02X}(CPUState *cpu) {{\n  printf(\"Opcode 0x{i:02X} not implemented yet.\\n\");   // TODO: implementation de l'opcode 0x{i:02X}\n}}\n")