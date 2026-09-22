package dram

import (
	"errors"
	"uPIMulator/src/abi/encoding"
	"uPIMulator/src/misc"
)

type Wordline struct {
	address int64
	size    int64

	byte_stream *encoding.ByteStream
}

func (this *Wordline) Init(address int64, size int64) {
	config_loader := new(misc.ConfigLoader)
	config_loader.Init()

	mram_data_size := int64(config_loader.MramDataWidth() / 8)

	if address < 0 {
		err := errors.New("address < 0")
		panic(err)
	} else if size <= 0 {
		err := errors.New("size <= 0")
		panic(err)
	} else if size%mram_data_size != 0 {
		err := errors.New("size is not aligned with MRAM data size")
		panic(err)
	}

	this.address = address
	this.size = size

	this.byte_stream = new(encoding.ByteStream)
	this.byte_stream.InitSize(size)
}

func (this *Wordline) Fini() {
}

func (this *Wordline) Read() *encoding.ByteStream {
	return this.byte_stream.Clone()
}

func (this *Wordline) Write(byte_stream *encoding.ByteStream) {
	if this.size != byte_stream.Size() {
		err := errors.New("wordline's size != byte stream's size")
		panic(err)
	}

	this.byte_stream.CopyFrom(0, byte_stream, 0, byte_stream.Size())
}

// WriteRange puts n bytes of src into this row at offset, without reading the
// row first. ReadRange is its opposite. Together they let a host transfer move
// a wordline at a time instead of a byte at a time.
func (this *Wordline) WriteRange(
	offset int64,
	src *encoding.ByteStream,
	src_offset int64,
	n int64,
) {
	this.byte_stream.CopyFrom(offset, src, src_offset, n)
}

func (this *Wordline) ReadRange(
	offset int64,
	dst *encoding.ByteStream,
	dst_offset int64,
	n int64,
) {
	dst.CopyFrom(dst_offset, this.byte_stream, offset, n)
}
