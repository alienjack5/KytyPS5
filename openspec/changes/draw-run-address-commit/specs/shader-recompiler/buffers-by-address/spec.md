# Spec Delta

## Purpose

How a compiled program reads a read-only buffer slot through a device address the host supplies per
draw instead of a bound descriptor: which slots qualify, the per-draw binding table the program
reads, the bounds behaviour, the equality with descriptor reads, the cache-key change and the switch.

## ADDED Requirements

### Requirement: Addressable slots are a function of the compiled program
With `KYTY_BUFFERS_BY_ADDRESS=1`, the recompiler SHALL classify a buffer slot as addressable when
the program only reads it, never through a formatted (typed) access, and it is neither a
mip-statistics nor an indirect-parameter buffer; a uniform-view slot (constant-only reads) SHALL
be addressable only with `KYTY_BUFFERS_BY_ADDRESS_UNIFORM=1`. The classification SHALL depend on
the program alone, so a program's descriptor set shape is fixed across draws.

#### Scenario: Written or formatted slot
- **WHEN** a program stores to a slot, performs an atomic on it, or reads it with a typed buffer load
- **THEN** the slot keeps its descriptor binding exactly as without the option

#### Scenario: Option off
- **WHEN** `KYTY_BUFFERS_BY_ADDRESS` is unset, empty, `0` or `off`
- **THEN** the program compiles and binds exactly as before this capability, with the same cache key as before

### Requirement: The binding table is the program's per-draw input
A stage with addressable slots SHALL read its per-draw buffer data from a binding table at a device
address delivered in its push constants, and SHALL have no `ShaderData` or `FlattenedSrt`
descriptor; a program whose push constants cannot hold the table pointer SHALL compile without
addressable slots.

#### Scenario: Table layout
- **WHEN** a stage has addressable slots
- **THEN** its table holds one entry per addressable slot (device address and size in bytes; a null slot has address 0 and size 0), then the stage's shader data, then its flattened SRT when the program has one, and the table pointer's dwords follow the mesh and clip-plane dwords in the push constants

#### Scenario: Table pointer does not fit
- **WHEN** a program's push constants have no room for the table pointer after its mesh and clip-plane dwords
- **THEN** every slot of that program stays descriptor-bound and the program is counted as such

### Requirement: Address reads equal descriptor reads
A read of an addressable slot SHALL return the same value the descriptor-bound read of the same
slot returns from the same bytes: an element whose byte range lies inside `[0, size)` reads the
bytes at `address + offset`; an element that is not wholly inside reads zero; unaligned and
dword-straddling accesses, stride and swizzle handling are those of the descriptor path.

#### Scenario: Out-of-range element
- **WHEN** a shader reads an element whose last byte is at or past `size` of its slot entry
- **THEN** the read returns zero, as the descriptor path's in-bounds predicate does

#### Scenario: Compute test selector
- **WHEN** the shader recompiler compute tests run their by-address selector
- **THEN** every case compares the by-address program's output with the descriptor program's output on the same inputs, including the last dword, a dword straddle, unaligned bytes, size 0, a size that is not a dword multiple and a null slot, and they are equal

### Requirement: The option is part of the cache key
The shader and pipeline caches SHALL key a compiled program on the by-address option and, while it
is on, on the uniform option, so switching either never serves a program compiled under the other;
with the by-address option off, the uniform option SHALL NOT change the key.

#### Scenario: Switch flipped between boots
- **WHEN** the emulator starts with a different value of `KYTY_BUFFERS_BY_ADDRESS` than the cache was written with
- **THEN** the affected programs are recompiled and the stored ones are not used
