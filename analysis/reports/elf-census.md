# ELF census — `libasphalt6.so`

- ELF type `3`, machine `40`, entry `0x4233a0`, flags `0x5000002`
- Primary function symbol source: `.symtab` (`.dynsym` also has 26,638 functions)

## Sections

| Section | Address | Size | Role |
|---|---|---|---|
| `.hash` | `0xd4` | 183,128 | symbol hash |
| `.dynsym` | `0x2cc2c` | 469,904 | dynamic symbols |
| `.dynstr` | `0x9f7bc` | 1,950,559 | dynamic strings |
| `.rel.dyn` | `0x27bb1c` | 1,728,984 | relocations |
| `.rel.plt` | `0x421cf4` | 2,312 | PLT relocations |
| `.plt` | `0x4225fc` | 3,488 | PLT stubs |
| `.text` | `0x4233a0` | 7,465,384 | code |
| `.rodata` | `0xb41d48` | 4,630,336 | read-only data |
| `.ARM.extab` | `0xfac488` | 10,232 | exception unwind tables |
| `.ARM.exidx` | `0xfaec80` | 237,544 | exception index |
| `.init_array` | `0xfe9c68` | 1,568 | global constructors |
| `.data.rel.ro` | `0xfea288` | 302,216 | vtables / RTTI |
| `.dynamic` | `0x1033f10` | 248 |  |
| `.got` | `0x1034008` | 10,656 | global offset table |
| `.data` | `0x10369a8` | 724,224 | initialised globals |
| `.bss` | `0x10e76a8` | 516,640 | zero-initialised globals |
| `.comment` | `0x0` | 12,528 |  |
| `.ARM.attributes` | `0x0` | 49 |  |
| `.shstrtab` | `0x0` | 178 |  |
| `.symtab` | `0x0` | 2,244,352 | full symbol table |
| `.strtab` | `0x0` | 2,124,427 | symbol strings |

## Symbols (.symtab)

| Kind | Count |
|---|---|
| Functions (defined) | 29,810 |
| — ARM entry points (even) | 28,708 |
| — Thumb entry points (odd) | 1,102 |
| — with non-zero size | 29,795 |
| — zero-size (aliases/thunks) | 15 |
| Objects (defined) | 49,352 |
| Undefined dynamic symbols | 301 |

## Function sizes (non-zero)

- total 7,471,292 bytes, mean 250 B, median 72 B
- largest 29,368 B

| Size bucket (bytes) | Functions | Bytes |
|---|---|---|
| <=16 | 3,578 | 34,900 |
| 17-64 | 10,405 | 415,798 |
| 65-256 | 10,117 | 1,361,528 |
| 257-1024 | 4,456 | 2,195,638 |
| 1025-4096 | 1,046 | 1,837,744 |
| >4096 | 193 | 1,625,684 |

## Namespace groups (top 25)

| Namespace | Functions |
|---|---|
| `(mangled)` | 6,833 |
| `(c symbols)` | 2,429 |
| `std::std::priv` | 1,827 |
| `std::std::vector` | 1,000 |
| `std` | 610 |
| `gameswf::array` | 331 |
| `glitch::video::detail` | 255 |
| `glitch::io::CAttributes` | 200 |
| `glitch::collada::animation_track` | 178 |
| `Singleton` | 176 |
| `gameswf::hash` | 147 |
| `__static_initialization_and_destruction_0` | 135 |
| `std::std::reverse_iterator` | 133 |
| `glitch::core::detail` | 131 |
| `std::std::map` | 118 |
| `glitch::video::SVertexStream` | 113 |
| `glitch::video::CNullDriver` | 99 |
| `boost::intrusive_ptr` | 95 |
| `glitch::video::CCommonGLDriver` | 92 |
| `glitch::scene::CSceneManager` | 91 |
| `std::std::__destroy_range_aux` | 88 |
| `std::std` | 85 |
| `std::std::__stlport_class` | 84 |
| `std::std::_Destroy_Range` | 80 |
| `std::std::__destroy_range` | 80 |

## Runtime structures

| Structure | Count / size |
|---|---|
| `.ARM.exidx` rows | 29,693 |
| `.ARM.extab` size | 10,232 B |
| `.init_array` constructors | 392 |

## Largest functions

| Address | Size | Symbol |
|---|---|---|
| `0x7d0f50` (arm) | 29,368 | `_ZNK7gameswf13action_buffer7executeEPNS_14as_environmentEiiPNS_8as_valueERKNS_5arrayINS_16with_stack_entryEEEb` |
| `0x9eb344` (arm) | 26,392 | `_ZN6glitch5video12pixel_format12_GLOBAL__N_113convertPackedENS0_14E_PIXEL_FORMATEPKvjS3_Pvjjjb` |
| `0xa00314` (arm) | 17,968 | `_ZN6glitch5video24guessShaderParameterTypeEPKc` |
| `0x9e3d10` (arm) | 15,144 | `_ZN6glitch5video12pixel_format12_GLOBAL__N_117convertPackedImplIhhEEbNS0_14E_PIXEL_FORMATEPKT_jS4_PT0_jjjb` |
| `0x9e7838` (arm) | 15,116 | `_ZN6glitch5video12pixel_format12_GLOBAL__N_117convertPackedImplIttEEbNS0_14E_PIXEL_FORMATEPKT_jS4_PT0_jjjb` |
| `0x9dcda4` (arm) | 15,100 | `_ZN6glitch5video12pixel_format12_GLOBAL__N_117convertPackedImplIjjEEbNS0_14E_PIXEL_FORMATEPKT_jS4_PT0_jjjb` |
| `0x90411c` (arm) | 14,292 | `TT_RunIns` |
| `0x7f3be8` (arm) | 13,200 | `_ZN7gameswf15log_disasm_avm2ERKNS_6membufEPKNS_7abc_defE` |
| `0x50dfe4` (arm) | 13,016 | `_ZN7RaceCarC2EiP10TrackSceneib12eControlTypebbPiiP10PlayerInfoPb` |
| `0x5112bc` (arm) | 13,016 | `_ZN7RaceCarC1EiP10TrackSceneib12eControlTypebbPiiP10PlayerInfoPb` |
| `0x72a740` (arm) | 12,448 | `_ZN7gameswf20morph2_character_def4readEPNS_6streamEibPNS_20movie_definition_subE` |
| `0x9f1a5c` (arm) | 10,432 | `_ZN6glitch5video12pixel_format7convertENS0_14E_PIXEL_FORMATEPKvjS2_Pvjjjb` |
| `0xa4b6b8` (arm) | 10,368 | `_ZN6glitch7collada33CGlitchNewParticleSystemSceneNode18initParticleSystemEPNS_5video12IVideoDriverEb` |
| `0xab5e64` (arm) | 10,032 | `cff_decoder_parse_charstrings` |
| `0x55c68c` (arm) | 9,800 | `_Z41__static_initialization_and_destruction_0ii` |
| `0x532f58` (arm) | 9,796 | `_Z41__static_initialization_and_destruction_0ii` |
| `0x46dec0` (arm) | 9,752 | `_Z41__static_initialization_and_destruction_0ii` |
| `0x7de490` (arm) | 9,676 | `_ZN7gameswf13as_3_function7executeERNS_5arrayINS_8as_valueEEES4_S4_PS2_` |
| `0x447b30` (arm) | 9,672 | `_Z41__static_initialization_and_destruction_0ii` |
| `0x47aff0` (arm) | 9,672 | `_Z41__static_initialization_and_destruction_0ii` |

## Undefined dynamic symbols by prefix

| Prefix | Count |
|---|---|
| `pthread` | 28 |
| `__c` | 6 |
| `SL` | 3 |
| `__a` | 3 |
| `__s` | 3 |
| `sched` | 2 |
| `inet` | 2 |
| `localtime` | 2 |
| `asinf` | 1 |
| `glCompressedTexSubImage2D` | 1 |
| `glDrawArrays` | 1 |
| `_toupper_tab_` | 1 |
| `atan2f` | 1 |
| `__e` | 1 |
| `glSampleCoverage` | 1 |
| `fgetpos` | 1 |
| `glBindBuffer` | 1 |
| `iswprint` | 1 |
| `connect` | 1 |
| `towupper` | 1 |
