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

### Requirement: The address entries are part of the stage's shader data
A stage with addressable slots SHALL carry one address entry per addressable slot after the
mip-statistics fields of its per-draw shader data, and SHALL deliver that shader data in the first
of three placements that fits the shared push-constant range: pushed, through a two-dword pointer,
or as the `ShaderData` descriptor.

#### Scenario: Entry layout
- **WHEN** a slot is addressable
- **THEN** its entry holds the device address low and high dwords, the size in bytes and a flags dword, and a null slot has address 0 and size 0

#### Scenario: Data fits the push range
- **WHEN** a stage's shader data with its address entries fits the remaining push-constant dwords
- **THEN** the entries are pushed with the rest of the shader data and the stage has no `ShaderData` descriptor

#### Scenario: Only the pointer fits
- **WHEN** the shader data does not fit the remaining push-constant dwords but two dwords do
- **THEN** the stage's uploaded shader data is reached through the pointer in those two dwords, the stage has no `ShaderData` descriptor, and the pushed cursor advances past the pointer

#### Scenario: Nothing fits
- **WHEN** neither the shader data nor two dwords fit the remaining push-constant dwords
- **THEN** the stage binds its shader data as the `ShaderData` descriptor, its slots stay addressable, and the pushed cursor does not move

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
