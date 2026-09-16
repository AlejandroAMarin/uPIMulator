package host

import (
	"path/filepath"
	"strconv"
	"strings"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/misc"
)

// Program is a linked DPU image together with its symbol tables: everything the
// simulator needs to bring a DPU up.
//
// The linker builds all of this in memory and then dumps it to bin_dirpath. The
// command line path reads it back with LoadProgramFromDir. A caller that drives
// the simulator directly hands over a Program instead, so no files are needed.
type Program struct {
	// Addresses maps a label to its address. This is what a host-side
	// dpu_copy_to(dpu, "SYMBOL", ...) resolves against. Local symbols carry a
	// "<relocatable>." prefix; global symbols keep their plain name.
	Addresses map[string]int64

	// Values maps a linker constant to its value, notably __sys_used_mram_end,
	// which is the base of the MRAM heap.
	Values map[string]int64

	Atomic *encoding.ByteStream
	Iram   *encoding.ByteStream
	Wram   *encoding.ByteStream
	Mram   *encoding.ByteStream
}

func LoadProgramFromDir(bin_dirpath string) *Program {
	program := new(Program)

	program.Addresses = LoadSymbolTable(filepath.Join(bin_dirpath, "addresses.txt"))
	program.Values = LoadSymbolTable(filepath.Join(bin_dirpath, "values.txt"))

	program.Atomic = LoadByteStream(filepath.Join(bin_dirpath, "atomic.bin"))
	program.Iram = LoadByteStream(filepath.Join(bin_dirpath, "iram.bin"))
	program.Wram = LoadByteStream(filepath.Join(bin_dirpath, "wram.bin"))
	program.Mram = LoadByteStream(filepath.Join(bin_dirpath, "mram.bin"))

	return program
}

// LoadSymbolTable reads a "name: value" table as dumped by
// Executable.DumpAddresses and LinkerScript.DumpValues.
func LoadSymbolTable(path string) map[string]int64 {
	file_scanner := new(misc.FileScanner)
	file_scanner.Init(path)

	lines := file_scanner.ReadLines()

	table := make(map[string]int64, 0)

	for _, line := range lines {
		words := strings.Split(line, ":")

		name := words[0]
		value, err := strconv.ParseInt(words[1][1:], 10, 64)

		if err != nil {
			panic(err)
		}

		table[name] = value
	}

	return table
}

// LoadByteStream reads a memory image. The files hold one decimal byte per
// line, not raw bytes.
func LoadByteStream(path string) *encoding.ByteStream {
	file_scanner := new(misc.FileScanner)
	file_scanner.Init(path)
	lines := file_scanner.ReadLines()

	byte_stream := new(encoding.ByteStream)
	byte_stream.Init()

	for _, line := range lines {
		value, err := strconv.Atoi(line)

		if err != nil {
			panic(err)
		}

		byte_stream.Append(uint8(value))
	}

	return byte_stream
}
