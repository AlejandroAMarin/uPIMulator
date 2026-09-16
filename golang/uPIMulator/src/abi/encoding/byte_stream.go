package encoding

type ByteStream struct {
	bytes []uint8
}

func (this *ByteStream) Init() {
	this.bytes = make([]uint8, 0)
}

// InitSize allocates size zeroed bytes in one go.
//
// Building a fixed-size buffer with Init plus a loop of Append costs a chain of
// reallocations and leaves the slice over-allocated, because Go rounds capacity
// up as it grows. At MRAM scale that overshoot was tens of megabytes per DPU.
func (this *ByteStream) InitSize(size int64) {
	this.bytes = make([]uint8, size)
}

// Clone returns an independent copy.
func (this *ByteStream) Clone() *ByteStream {
	byte_stream := new(ByteStream)
	byte_stream.bytes = make([]uint8, len(this.bytes))
	copy(byte_stream.bytes, this.bytes)
	return byte_stream
}

func (this *ByteStream) Size() int64 {
	return int64(len(this.bytes))
}

func (this *ByteStream) Get(pos int) uint8 {
	return this.bytes[pos]
}

func (this *ByteStream) Set(pos int, value uint8) {
	this.bytes[pos] = value
}

func (this *ByteStream) Append(value uint8) {
	this.bytes = append(this.bytes, value)
}

func (this *ByteStream) Merge(byte_stream *ByteStream) {
	for i := int64(0); i < byte_stream.Size(); i++ {
		value := byte_stream.Get(int(i))
		this.Append(value)
	}
}
