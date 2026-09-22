package dram

import (
	"errors"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/misc"
)

type Mram struct {
	address int64
	size    int64

	wordline_size int64
	wordlines     map[int]*Wordline
}

func (this *Mram) Init(command_line_parser *misc.CommandLineParser) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	this.address = config_loader.MramOffset()
	this.size = config_loader.MramSize()
	this.wordline_size = command_line_parser.IntParameter("wordline_size")

	if this.wordline_size <= 0 {
		err := errors.New("wordline size <= 0")
		panic(err)
	} else if this.address%this.wordline_size != 0 {
		err := errors.New("address is not aligned with wordline size")
		panic(err)
	} else if this.size%this.wordline_size != 0 {
		err := errors.New("size is not aligned with wordline size")
		panic(err)
	}

	// Wordlines are created when first written, not up front. A DPU has 64 MB
	// of MRAM but a workload typically touches a small part of it, and
	// allocating all 65536 rows eagerly cost about 90 MB per simulated DPU,
	// which is what limited how many DPUs fit in host memory.
	this.wordlines = make(map[int]*Wordline, 0)
}

func (this *Mram) Fini() {
	for _, wordline := range this.wordlines {
		wordline.Fini()
	}
}

func (this *Mram) Address() int64 {
	return this.address
}

func (this *Mram) Size() int64 {
	return this.size
}

func (this *Mram) Read(address int64) *encoding.ByteStream {
	wordline, found := this.wordlines[this.Index(address)]

	// Memory that was never written reads as zero, so an absent wordline does
	// not have to be created just to be read.
	if !found {
		byte_stream := new(encoding.ByteStream)
		byte_stream.InitSize(this.wordline_size)
		return byte_stream
	}

	return wordline.Read()
}

func (this *Mram) Write(address int64, byte_stream *encoding.ByteStream) {
	this.Wordline(address).Write(byte_stream)
}

// WriteRange puts n bytes of src into the row holding address, at offset within
// it. It does not read the row first, so a transfer that covers whole rows
// never copies what it is about to overwrite.
func (this *Mram) WriteRange(
	address int64,
	offset int64,
	src *encoding.ByteStream,
	src_offset int64,
	n int64,
) {
	this.Wordline(address).WriteRange(offset, src, src_offset, n)
}

// ReadRange takes n bytes out of the row holding address. A row that was never
// written reads as zero, and dst arrives zeroed, so there is nothing to do.
func (this *Mram) ReadRange(
	address int64,
	offset int64,
	dst *encoding.ByteStream,
	dst_offset int64,
	n int64,
) {
	wordline, found := this.wordlines[this.Index(address)]

	if !found {
		return
	}

	wordline.ReadRange(offset, dst, dst_offset, n)
}

// Wordline returns the row holding address, creating it on first write.
func (this *Mram) Wordline(address int64) *Wordline {
	index := this.Index(address)

	wordline, found := this.wordlines[index]

	if !found {
		wordline = new(Wordline)
		wordline.Init(this.address+int64(index)*this.wordline_size, this.wordline_size)
		this.wordlines[index] = wordline
	}

	return wordline
}

// NumResidentWordlines reports how many rows have been written, for measuring
// the memory a workload actually needs.
func (this *Mram) NumResidentWordlines() int {
	return len(this.wordlines)
}

func (this *Mram) Index(address int64) int {
	if address < this.address {
		err := errors.New("address < MRAM offset")
		panic(err)
	} else if address+this.wordline_size > this.address+this.size {
		err := errors.New("address + wordline size > MRAM offset + MRAM size")
		panic(err)
	} else if address%this.wordline_size != 0 {
		err := errors.New("address is not aligned with wordline size")
		panic(err)
	}

	return int((address - this.address) / this.wordline_size)
}
