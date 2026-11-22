// Package elfload turns a DPU ELF, the file the machine runs, into a program
// the simulator can load, so that both sides execute the same instructions.
//
// uPIMulator normally links a kernel itself, from assembly text, against its
// own copy of the runtime. That copy is not the SDK's (mem_alloc_nolock was one
// instruction shorter, perfcounter_config three), and a kernel binary found at
// a host's path need not be the build the simulator linked. Loading the ELF
// removes both differences.
//
// The ELF is linked against UPMEM's address map, so the simulator must run it
// with misc.UseHardwareMemoryMap(true): IRAM addresses are instruction
// indices, WRAM starts at 0, MRAM at 0x08000000.
//
// Instructions are decoded by the SDK's disassembler, /usr/bin/llvm-objdump
// (UPIM_OBJDUMP overrides it), and read back with uPIMulator's own assembly
// grammar. Branch targets it prints as byte addresses or symbol names become
// instruction indices; every other number becomes decimal, because the
// grammar's hexadecimal token has no letter digits.
package elfload

import (
	"debug/elf"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/linker/kernel"
	"uPIMulator/src/linker/lexer"
	"uPIMulator/src/linker/logic"
	"uPIMulator/src/linker/parser"
	"uPIMulator/src/misc"
	"uPIMulator/src/simulator/host"
)

// UPMEM's address map, as a DPU ELF is linked.
const (
	iramBase   = 0x80000000
	iramStep   = 8
	atomicBase = 0xf0000000
	mramBase   = 0x08000000
	mramSize   = 64 * 1024 * 1024
	wramSize   = 64 * 1024
	atomicSize = 256
)

var (
	disassembledLine = regexp.MustCompile(`^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} ){8})\s*(.*)$`)
	hexNumber        = regexp.MustCompile(`^-?0x[0-9a-f]+$`)
)

// Load reads the DPU ELF at path.
func Load(path string) (program *host.Program, err error) {
	defer func() {
		if r := recover(); r != nil {
			program = nil
			err = fmt.Errorf("elfload: %s: %v", path, r)
		}
	}()

	file, err := elf.Open(path)
	if err != nil {
		return nil, fmt.Errorf("elfload: %v", err)
	}
	defer file.Close()

	symbols, err := file.Symbols()
	if err != nil {
		return nil, fmt.Errorf("elfload: %s: %v", path, err)
	}

	text_symbols := make(map[string]int64)
	table := make(map[string]int64)
	for _, symbol := range symbols {
		if symbol.Name == "" {
			continue
		}
		value := int64(symbol.Value)
		switch {
		case value >= iramBase:
			value = (value - iramBase) / iramStep
			if int(symbol.Section) < len(file.Sections) &&
				file.Sections[symbol.Section].Name == ".text" {
				text_symbols[symbol.Name] = value
			}
		case value >= atomicBase:
			value -= atomicBase
		}
		table[symbol.Name] = value
	}

	lines, err := disassemble(path, text_symbols)
	if err != nil {
		return nil, err
	}

	program = new(host.Program)
	program.Addresses = table
	program.Values = table
	program.Iram = assemble(lines)
	program.Wram, program.Mram, program.Atomic = images(file)

	return program, nil
}

// disassemble returns .text one instruction a line, with numeric operands.
func disassemble(path string, text_symbols map[string]int64) ([]string, error) {
	objdump := os.Getenv("UPIM_OBJDUMP")
	if objdump == "" {
		objdump = "/usr/bin/llvm-objdump"
	}

	out, err := exec.Command(objdump, "-d", "-j", ".text", path).Output()
	if err != nil {
		return nil, fmt.Errorf("elfload: %s -d %s: %v", objdump, path, err)
	}

	lines := make([]string, 0)
	for _, raw := range strings.Split(string(out), "\n") {
		match := disassembledLine.FindStringSubmatch(raw)
		if match == nil {
			continue
		}

		address, _ := strconv.ParseInt(match[1], 16, 64)
		if want := int64(iramBase + iramStep*len(lines)); address != want {
			return nil, fmt.Errorf("elfload: %s: instruction at %#x, expected %#x", path, address, want)
		}

		lines = append(lines, normalize(match[3], text_symbols))
	}

	if len(lines) == 0 {
		return nil, fmt.Errorf("elfload: %s: no instructions in .text", path)
	}
	return lines, nil
}

// normalize rewrites one disassembled instruction's operands as numbers the
// grammar reads: a code address or a code symbol becomes an instruction index,
// any other hexadecimal number becomes decimal.
func normalize(text string, text_symbols map[string]int64) string {
	mnemonic, rest, found := strings.Cut(strings.TrimSpace(text), " ")
	if !found {
		return mnemonic
	}

	operands := strings.Split(rest, ",")
	for i, operand := range operands {
		operand = strings.TrimSpace(operand)

		if index, is_symbol := text_symbols[operand]; is_symbol {
			operand = strconv.FormatInt(index, 10)
		} else if hexNumber.MatchString(operand) {
			value, err := strconv.ParseInt(operand, 0, 64)
			if err != nil {
				panic(fmt.Sprintf("operand %q: %v", operand, err))
			}
			if value >= iramBase && value < atomicBase {
				value = (value - iramBase) / iramStep
			}
			operand = strconv.FormatInt(value, 10)
		}

		operands[i] = operand
	}

	return mnemonic + " " + strings.Join(operands, ", ")
}

// assemble reads the lines with uPIMulator's grammar and returns their
// encodings in order, one instruction for each index.
func assemble(lines []string) *encoding.ByteStream {
	scratch, err := os.CreateTemp("", "upim-elf-*.S")
	if err != nil {
		panic(err)
	}
	defer os.Remove(scratch.Name())

	source := ".text\n__elf_text:\n" + strings.Join(lines, "\n") + "\n"
	if _, err := scratch.WriteString(source); err != nil {
		panic(err)
	}
	scratch.Close()

	lexer_ := new(lexer.Lexer)
	lexer_.Init()
	token_stream := lexer_.Lex(scratch.Name())

	parser_ := new(parser.Parser)
	parser_.Init()
	ast := parser_.Parse(token_stream)

	executable := new(kernel.Executable)
	executable.Init("elf")
	executable.SetPath(scratch.Name())
	executable.SetTokenStream(token_stream)
	executable.SetAst(ast)

	label_assigner := new(logic.LabelAssigner)
	label_assigner.Init()
	label_assigner.Assign(executable)

	instruction_assigner := new(logic.InstructionAssigner)
	instruction_assigner.Init(nil)
	instruction_assigner.Assign(executable)

	label := executable.Label("__elf_text")
	if label == nil {
		panic("the instructions' label is missing")
	}

	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	byte_stream := label.ToByteStream()
	width := int64(config_loader.IramDataWidth() / 8)
	if got := byte_stream.Size() / width; got != int64(len(lines)) {
		panic(fmt.Sprintf("%d instructions assembled from %d lines", got, len(lines)))
	}
	if int64(len(lines)) > config_loader.IramInstructions() {
		panic(fmt.Sprintf("%d instructions do not fit IRAM", len(lines)))
	}

	return byte_stream
}

// images lays the allocated sections out as the machine's memories: WRAM from
// 0, MRAM from 0x08000000, and the atomic bits, which start clear.
func images(file *elf.File) (wram, mram, atomic *encoding.ByteStream) {
	wram_bytes := make([]byte, 0)
	mram_bytes := make([]byte, 0)

	place := func(image []byte, offset int64, data []byte, size int64) []byte {
		if need := offset + size; int64(len(image)) < need {
			image = append(image, make([]byte, need-int64(len(image)))...)
		}
		copy(image[offset:], data)
		return image
	}

	for _, section := range file.Sections {
		if section.Flags&elf.SHF_ALLOC == 0 || section.Size == 0 {
			continue
		}

		address := int64(section.Addr)
		size := int64(section.Size)

		var data []byte
		if section.Type != elf.SHT_NOBITS {
			var err error
			if data, err = section.Data(); err != nil {
				panic(fmt.Sprintf("section %s: %v", section.Name, err))
			}
		}

		switch {
		case address >= iramBase:
			// .text, which assemble already has, and .atomic, which starts
			// clear on the machine.
		case address >= mramBase:
			if address-mramBase+size > mramSize {
				panic(fmt.Sprintf("section %s does not fit MRAM", section.Name))
			}
			mram_bytes = place(mram_bytes, address-mramBase, data, size)
		default:
			if address+size > wramSize {
				panic(fmt.Sprintf("section %s does not fit WRAM", section.Name))
			}
			wram_bytes = place(wram_bytes, address, data, size)
		}
	}

	return stream(wram_bytes), stream(mram_bytes), stream(make([]byte, atomicSize))
}

func stream(data []byte) *encoding.ByteStream {
	byte_stream := new(encoding.ByteStream)
	byte_stream.InitSize(int64(len(data)))
	for i, b := range data {
		byte_stream.Set(i, b)
	}
	return byte_stream
}
