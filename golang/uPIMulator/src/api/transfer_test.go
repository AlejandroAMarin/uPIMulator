package api

import (
	"math"
	"testing"
)

// transferPoint is one median a hardware sweep measured: a dpu_push_xfer of
// bytes to or from each of dpus DPUs, which lay on ranks ranks, the fullest
// holding rank_dpus of them.
type transferPoint struct {
	dpus      int
	ranks     int
	rank_dpus int
	to_dpu    bool
	bytes     int64
	median_us float64
}

// transferPoints are the medians of runner/tests/perf/xfer_host.c (one rank, 1
// to 64 DPUs, two runs) and runner/tests/perf/rank_host.c (64 to 1024 DPUs on
// 1 to 16 ranks, three runs), measured on 2026-10-02 on this machine, up to
// 256 KB per DPU.
var transferPoints = []transferPoint{
	{1, 1, 1, true, 8, 31},
	{1, 1, 1, true, 512, 26},
	{1, 1, 1, true, 4096, 26},
	{1, 1, 1, true, 32768, 50},
	{1, 1, 1, true, 262144, 295},
	{2, 1, 2, true, 8, 30},
	{2, 1, 2, true, 512, 27},
	{2, 1, 2, true, 4096, 27},
	{2, 1, 2, true, 32768, 51},
	{2, 1, 2, true, 262144, 295},
	{4, 1, 4, true, 8, 29},
	{4, 1, 4, true, 512, 26},
	{4, 1, 4, true, 4096, 24},
	{4, 1, 4, true, 32768, 49},
	{4, 1, 4, true, 262144, 294},
	{8, 1, 8, true, 8, 28},
	{8, 1, 8, true, 512, 20},
	{8, 1, 8, true, 4096, 23},
	{8, 1, 8, true, 32768, 52},
	{8, 1, 8, true, 262144, 307},
	{16, 1, 16, true, 8, 28},
	{16, 1, 16, true, 512, 26},
	{16, 1, 16, true, 4096, 25},
	{16, 1, 16, true, 32768, 74},
	{16, 1, 16, true, 262144, 469},
	{32, 1, 32, true, 8, 24},
	{32, 1, 32, true, 512, 27},
	{32, 1, 32, true, 4096, 34},
	{32, 1, 32, true, 32768, 112},
	{32, 1, 32, true, 262144, 776},
	{64, 1, 64, true, 8, 33},
	{64, 1, 64, true, 512, 54},
	{64, 1, 64, true, 4096, 73},
	{64, 1, 64, true, 32768, 226},
	{64, 1, 64, true, 262144, 2008},
	{65, 2, 33, true, 512, 76},
	{65, 2, 33, true, 32768, 310},
	{65, 2, 33, true, 262144, 3184},
	{128, 2, 64, true, 512, 80},
	{128, 2, 64, true, 32768, 345},
	{128, 2, 64, true, 262144, 4990},
	{199, 4, 55, true, 512, 103},
	{199, 4, 55, true, 32768, 484},
	{199, 4, 55, true, 262144, 5457},
	{256, 4, 64, true, 512, 111},
	{256, 4, 64, true, 32768, 592},
	{256, 4, 64, true, 262144, 5770},
	{512, 8, 64, true, 512, 203},
	{512, 8, 64, true, 32768, 1135},
	{512, 8, 64, true, 262144, 13817},
	{1024, 16, 64, true, 512, 316},
	{1024, 16, 64, true, 32768, 2443},
	{1024, 16, 64, true, 262144, 19363},
	{1, 1, 1, false, 8, 32},
	{1, 1, 1, false, 512, 28},
	{1, 1, 1, false, 4096, 39},
	{1, 1, 1, false, 32768, 164},
	{1, 1, 1, false, 262144, 1144},
	{2, 1, 2, false, 8, 33},
	{2, 1, 2, false, 512, 27},
	{2, 1, 2, false, 4096, 39},
	{2, 1, 2, false, 32768, 163},
	{2, 1, 2, false, 262144, 1138},
	{4, 1, 4, false, 8, 29},
	{4, 1, 4, false, 512, 25},
	{4, 1, 4, false, 4096, 37},
	{4, 1, 4, false, 32768, 167},
	{4, 1, 4, false, 262144, 1168},
	{8, 1, 8, false, 8, 28},
	{8, 1, 8, false, 512, 23},
	{8, 1, 8, false, 4096, 37},
	{8, 1, 8, false, 32768, 175},
	{8, 1, 8, false, 262144, 1239},
	{16, 1, 16, false, 8, 24},
	{16, 1, 16, false, 512, 28},
	{16, 1, 16, false, 4096, 38},
	{16, 1, 16, false, 32768, 179},
	{16, 1, 16, false, 262144, 1267},
	{32, 1, 32, false, 8, 26},
	{32, 1, 32, false, 512, 39},
	{32, 1, 32, false, 4096, 58},
	{32, 1, 32, false, 32768, 324},
	{32, 1, 32, false, 262144, 2448},
	{64, 1, 64, false, 8, 35},
	{64, 1, 64, false, 512, 86},
	{64, 1, 64, false, 4096, 110},
	{64, 1, 64, false, 32768, 632},
	{64, 1, 64, false, 262144, 5252},
	{65, 2, 33, false, 512, 112},
	{65, 2, 33, false, 32768, 987},
	{65, 2, 33, false, 262144, 8467},
	{128, 2, 64, false, 512, 135},
	{128, 2, 64, false, 32768, 1255},
	{128, 2, 64, false, 262144, 11222},
	{199, 4, 55, false, 512, 128},
	{199, 4, 55, false, 32768, 1279},
	{199, 4, 55, false, 262144, 12068},
	{256, 4, 64, false, 512, 141},
	{256, 4, 64, false, 32768, 2113},
	{256, 4, 64, false, 262144, 12937},
	{512, 8, 64, false, 512, 272},
	{512, 8, 64, false, 32768, 3558},
	{512, 8, 64, false, 262144, 36912},
	{1024, 16, 64, false, 512, 447},
	{1024, 16, 64, false, 32768, 5266},
	{1024, 16, 64, false, 262144, 44861},
}

// TestTransferModelAgainstHardware checks the transfer model against the
// machine. The bounds are what the model reached when it was fitted; a change
// that makes it worse than them fails here. go test -v prints every point.
func TestTransferModelAgainstHardware(t *testing.T) {
	set := &Set{config: DefaultConfig(), transfer_count: 1}
	mhz := float64(set.config.LogicFrequency)

	var one_rank, many_ranks []float64

	for _, point := range transferPoints {
		cycles := set.TransferCycles(point.dpus, point.ranks, point.rank_dpus, point.bytes, point.to_dpu)
		model_us := float64(cycles) / mhz
		e := math.Log(model_us / point.median_us)

		if point.ranks == 1 {
			one_rank = append(one_rank, e)
		} else {
			many_ranks = append(many_ranks, e)
		}

		t.Logf("%5d DPUs %2d ranks %-5v %7d B: hardware %8.0f us, model %8.0f us, %+4.0f%%",
			point.dpus, point.ranks, point.to_dpu, point.bytes, point.median_us, model_us,
			100*(model_us/point.median_us-1))
	}

	rms := func(errors []float64) float64 {
		sum := 0.0
		for _, e := range errors {
			sum += e * e
		}
		return math.Sqrt(sum / float64(len(errors)))
	}

	t.Logf("one rank: rms %.1f%% over %d points; many ranks: rms %.1f%% over %d points",
		100*rms(one_rank), len(one_rank), 100*rms(many_ranks), len(many_ranks))

	if rms(one_rank) > 0.25 {
		t.Errorf("one rank: rms log error %.3f, above 0.25", rms(one_rank))
	}

	if rms(many_ranks) > 0.36 {
		t.Errorf("many ranks: rms log error %.3f, above 0.36", rms(many_ranks))
	}
}

// TestTransferModelShape checks what the model must do whatever its numbers.
func TestTransferModelShape(t *testing.T) {
	set := &Set{config: DefaultConfig(), transfer_count: 1}

	// One rank: the rank count adds nothing.
	if set.TransferCycles(64, 1, 64, 4096, true) != set.TransferCycles(64, 0, 0, 4096, true) {
		t.Errorf("a one-rank transfer depends on how the rank is given")
	}

	// The same DPUs over more ranks: the setup grows, the rank limit falls.
	one := set.TransferCycles(128, 1, 128, 512, true)
	two := set.TransferCycles(128, 2, 64, 512, true)
	if two <= one {
		t.Errorf("a small transfer over two ranks (%d) is not dearer than over one (%d)", two, one)
	}

	// A large transfer to many ranks is limited by the host.
	host_off := *set
	host_off.config.HostWriteMbps = 0
	with_host := set.TransferCycles(1024, 16, 64, 262144, true)
	without := host_off.TransferCycles(1024, 16, 64, 262144, true)
	if with_host <= without {
		t.Errorf("the host limit does not bind on 16 ranks: %d against %d without it", with_host, without)
	}

	// Zero DPU or rank rates make transfers free, as before.
	free := *set
	free.config.DpuWriteMbps = 0
	if free.TransferCycles(64, 1, 64, 4096, true) != 0 {
		t.Errorf("a zero rate does not make the transfer free")
	}
}
