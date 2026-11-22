package sram

import (
	"errors"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/linker/kernel/instruction"
	"uPIMulator/src/misc"
)

type Iram struct {
	address int64
	size    int64

	byte_stream *encoding.ByteStream

	// decoded caches the instruction at each address. IRAM is written once at
	// load and then fetched every cycle, so decoding on every fetch was pure
	// repeated work, and the Word it allocates was a large share of the
	// garbage the simulation produced.
	decoded map[int64]*instruction.Instruction
}

func (this *Iram) Init() {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	this.address = config_loader.IramOffset()
	this.size = config_loader.IramSize()

	// The storage holds IramInstructions encodings of IramDataWidth bits
	// each, whatever step the program counter takes between them.
	this.byte_stream = new(encoding.ByteStream)
	this.byte_stream.InitSize(config_loader.IramInstructions() * int64(config_loader.IramDataWidth()/8))

	this.decoded = make(map[int64]*instruction.Instruction, 0)
}

func (this *Iram) Fini() {
}

func (this *Iram) Address() int64 {
	return this.address
}

func (this *Iram) Size() int64 {
	return this.size
}

func (this *Iram) Read(address int64) *instruction.Instruction {
	cached, found := this.decoded[address]

	if !found {
		config_loader := new(misc.ConfigLoader)
		config_loader.Init()

		iram_data_size := int64(config_loader.IramDataWidth() / 8)

		byte_stream := new(encoding.ByteStream)
		byte_stream.InitSize(iram_data_size)
		for i := int64(0); i < iram_data_size; i++ {
			index := this.Index(address) + int(i)

			byte_stream.Set(int(i), this.byte_stream.Get(index))
		}

		cached = new(instruction.Instruction)
		cached.Decode(byte_stream)

		this.decoded[address] = cached
	}

	// Hand back a copy, never the cached instruction itself. An Instruction is
	// immutable once decoded, but the logic keys its scoreboard by pointer, so
	// two tasklets executing the same address must hold distinct pointers.
	instruction_ := new(instruction.Instruction)
	*instruction_ = *cached
	return instruction_
}

func (this *Iram) Write(address int64, byte_stream *encoding.ByteStream) {
	// Writes only happen when a program is loaded, so dropping every cached
	// decode is simpler than working out which addresses this write covers.
	clear(this.decoded)

	for i := int64(0); i < byte_stream.Size(); i++ {
		index := this.Index(address) + int(i)

		this.byte_stream.Set(index, byte_stream.Get(int(i)))
	}
}

// Index is the byte offset in the storage of the instruction at address.
func (this *Iram) Index(address int64) int {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	stride := config_loader.IramStride()
	iram_data_size := int64(config_loader.IramDataWidth() / 8)

	if address < this.address {
		err := errors.New("address < IRAM offset")
		panic(err)
	} else if address+stride > this.address+this.size {
		err := errors.New("address >= IRAM offset + IRAM size")
		panic(err)
	}

	if (address-this.address)%stride != 0 {
		err := errors.New("addresses are not aligned with IRAM data size")
		panic(err)
	}

	return int((address - this.address) / stride * iram_data_size)
}
