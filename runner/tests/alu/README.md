# ALU, condition-code and DMA conformance tests

Runs uPIMulator against UPMEM's own functional simulator and diffs every result.
`runner/tests/fp` proved the double-precision path was broken; these tests are
what found the causes, and they are what keep them fixed.

    runner/tests/alu/run.sh

Both sides execute the same kernel sources. The reference uses
`backend=simulator` (`libdpufsim`), which links only against libc and cannot
reach a DPU rank, so neither side touches this machine's hardware.

## What it covers

`kernel.c` — what the ALU computes.

- Every register-register operation the linker's grammar accepts: `and`,
  `andn`, `nand`, `nor`, `nxor`, `or`, `orn`, `xor`, `add`, `sub`, `rsub`,
  `lsl`, `lslx`, `lsr`, `lsrx`, `asr`, `rol`, `ror`, `lsl1`, `lsl1x`, `lsr1`,
  `lsr1x`, `cmpb4`, and three `mul_*` forms — 26 operations over 16 operand
  pairs chosen to stress signs, zeroes, all-ones, and shift amounts above 31.
- The extended compare conditions `xgts`, `xles`, `xgtu`, `xleu`, `xz`, `xnz`
  over 12 64-bit operand pairs, including the signed and unsigned extremes.
- 64-bit `add`/`addc` and `sub`/`subc` pairs, which exercise the carry chain
  itself rather than just the conditions read from it.

`kernel_cond.c` — what the ALU decides, and the narrow accesses.

- Every condition `sub` can set (22) and every one `add` can set (14), over 20
  operand pairs. BIMSA branches on `jlts`/`jges`/`jgts`/`jles`, which are these.
- `lbu`, `lbs`, `lhu`, `lhs`, `extsb`, `extsh`, `extub`, `extuh`, and a byte
  store followed by a half store into the same word.
- An MRAM round trip.

`mram.c` — DMA addressing.

- An 8-byte transfer with each of the MRAM and WRAM addresses aligned and 4 mod
  8, in every combination.

Inline assembly is deliberate throughout: it pins the exact mnemonic under test
instead of whatever the compiler would pick for a C expression.

## Bugs it found

| Defect | Effect |
|---|---|
| DMA honoured the low 3 bits of both addresses; the hardware ignores them | every unaligned transfer moved a few bytes, losing the data. BIMSA reported distance 0 for alignments it had computed correctly |
| `xor` returned its second operand; `nxor` returned the complement of it | every `xor` with a non-zero first operand was wrong |
| `rol`/`ror` overwrote the rotated-in bits with ones | `rol`/`ror` behaved as `lsl1`/`lsr1` |
| the subtract carry was the borrow; the hardware reports its complement | every `c`/`nc` condition after a subtract was inverted |
| `sub`/`subc` computed signed overflow with the result's sign inverted | `xgts`/`xles` were wrong whenever the operands' signs differed |
| `subc` used the wrong borrow test when a borrow came in | wrong carry out of a multi-word subtract |
| `xgtu`/`xleu` were both set when the carry was set, and `xles` could never be set at all | 64-bit compares took the wrong branch |
| `pl`/`mi` tested the unsigned image of the result, so `mi` could never be set | every negative result looked positive |
| `ExecuteAddRrrci`/`ExecuteAddZrrci` discarded the ALU's overflow | `ov`/`nov` never fired after an add |
| `sh32`/`nsh32` were never set by any instruction | 64-bit shifts silently took the shift<32 path |
| `lsl1x`/`lsr1x` were not implemented | panic on use |
| `ExecuteSRrr` was not implemented | panic on `lsr.u dc, ra, rb` |

## Limitations

- `hash` and `sats` are still unimplemented. `hash` has no statement rule in the
  linker's grammar, so a kernel using it cannot be linked at all; the tests do
  not cover either.
- The operand tables are fixed, not random. They are chosen for coverage of the
  boundaries, not for exhaustiveness.
- `.section .stack_sizes` has no rule in the linker's grammar either, and clang
  has no flag to suppress it, so `run.sh` strips it with `sed` before linking.
