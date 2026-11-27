# Instruction census — `libasphalt6.so`

- Functions analysed (`.symtab`): **29,795**
- Instructions decoded: **1,545,201** (ARM: **1,496,208**, Thumb-2: **48,993**)
- Mean instructions/function: **51**, median **19**, max **2,409**
- Entry point: `0x4233a0`

## Condition codes

| CC | Instructions |
|---|---|
| AL | 1,390,198 |
| EQ | 73,853 |
| NE | 36,681 |
| LE | 7,848 |
| GT | 6,451 |
| LT | 5,930 |
| LO | 5,296 |
| HI | 4,260 |
| LS | 4,146 |
| GE | 3,362 |
| MI | 3,074 |
| HS | 2,933 |
| PL | 1,124 |
| VC | 31 |
| VS | 14 |

## Top 50 mnemonics

| Mnemonic | Count |
|---|---|
| `ldr` | 301,204 |
| `mov` | 259,741 |
| `add` | 164,079 |
| `str` | 133,496 |
| `bl` | 100,936 |
| `cmp` | 85,954 |
| `beq` | 43,016 |
| `b` | 36,730 |
| `vldr` | 29,356 |
| `sub` | 26,886 |
| `bne` | 20,650 |
| `vstr` | 19,877 |
| `strb` | 19,190 |
| `pop` | 16,276 |
| `ldrb` | 14,759 |
| `push` | 14,721 |
| `rsb` | 8,809 |
| `movw` | 8,595 |
| `ldm` | 8,544 |
| `andeq` | 8,532 |
| `bx` | 8,527 |
| `vmov` | 8,174 |
| `lsl` | 8,065 |
| `uxtb` | 5,939 |
| `vmul.f32` | 5,009 |
| `ble` | 4,972 |
| `moveq` | 4,832 |
| `subs` | 4,695 |
| `blx` | 4,525 |
| `movne` | 4,520 |
| `vmrs` | 4,437 |
| `ldrh` | 4,042 |
| `stm` | 3,940 |
| `mvn` | 3,751 |
| `blt` | 3,748 |
| `asr` | 3,629 |
| `strh` | 3,361 |
| `orr` | 3,281 |
| `cmn` | 3,281 |
| `movt` | 3,211 |
| `vcmpe.f32` | 3,168 |
| `bgt` | 3,042 |
| `and` | 3,011 |
| `vmla.f32` | 3,009 |
| `mul` | 2,922 |
| `adds` | 2,764 |
| `lsr` | 2,742 |
| `addne` | 2,723 |
| `bhi` | 2,668 |
| `ldrsb` | 2,555 |

## Categories

| Category | Instructions | Functions using it |
|---|---|---|
| VFP (any `v*`) | 92,557 | 3,813 |
| NEON-like | 10,021 | 1,265 |
| branches `b/bl` | 137,666 |  |
| indirect `bx/blx` | 13,052 |  |
| load/store `ldr/str` | 498,465 |  |
| block `ldm/stm` | 13,292 |  |
| stack `push/pop` | 31,522 |  |
| multiply `mul/mla/umull/smull` | 5,001 |  |
| atomics `ldrex/strex` | 0 | 0 |
| syscall `svc` | 2 | 0 |
| coprocessor `mcr/mrc` | 0 | 2 |
| saturating `q*` | 0 |  |
| bitfield `ubfx/sbfx/bfi/bfc` | 2,601 |  |

## Calls

| Kind | Count |
|---|---|
| Direct `bl` (immediate) | 101,848 |
| Indirect `blx`/register | 3,613 |
| `bx` / `ldr pc` sites | 73,010 |

## Most-called internal symbols (direct calls)

| Symbol | Calls |
|---|---|
| `sub_42297c` | 4,092 |
| `sub_4230cc` | 4,091 |
| `_ZN6glitch4core8vector3dIfEC1Efff` | 2,760 |
| `_ZdlPv` | 2,533 |
| `sub_422ef8` | 1,774 |
| `_Z10GlitchFreePv` | 1,740 |
| `sub_422aa8` | 961 |
| `_Znwj` | 934 |
| `_ZN7gameswf13free_internalEPvj` | 827 |
| `_ZN4Game7GetGameEv` | 685 |
| `_ZN6glitch4core8vector3dIfEC1Ev` | 633 |
| `_ZL7getRandv` | 540 |
| `_ZN7gameswf8as_value9drop_refsEv` | 501 |
| `sub_422bc8` | 458 |
| `___ZNSt12__node_alloc13_M_deallocateEPvj_veneer` | 451 |
| `_ZdaPv` | 447 |
| `_ZN6glitch4core8vector3dIfEC1ERKS2_` | 443 |
| `_Znaj` | 430 |
| `sub_4226d0` | 428 |
| `_Z11GlitchAllocjN6glitch6memory13E_MEMORY_HINTE` | 418 |
| `_ZN7gameswf11ref_counted8drop_refEv` | 409 |
| `sub_422e20` | 387 |
| `sub_42306c` | 386 |
| `_ZNK6glitch7collada18SAnimationAccessor15getDefaultValueEv` | 366 |
| `_ZN7gameswf9tu_string6resizeEi` | 351 |
| `sub_4230fc` | 346 |
| `sub_422ab4` | 343 |
| `_ZN9CMatching3GetEv` | 341 |
| `_ZNK6glitch7collada18SAnimationAccessor9getOutputEi` | 338 |
| `XP_API_MEMSET` | 333 |
| `ft_mem_free` | 333 |
| `_ZN6glitch4core8vector3dIfEaSERKS2_` | 329 |
| `_ZNK6glitch17IReferenceCounted4dropEv` | 321 |
| `_ZNK5boost13intrusive_ptrIN6glitch5video9CMaterialEEptEv` | 305 |
| `sub_422de4` | 283 |
| `_ZN14NetworkManager11GetInstanceEv` | 282 |
| `_Z7VoxFreePv` | 282 |
| `sub_422d0c` | 281 |
| `_ZNK7gameswf11ref_counted7add_refEv` | 275 |
| `sub_422a9c` | 274 |
