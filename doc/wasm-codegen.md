# Compiling C to WebAssembly with lcc-wasm

How the `-target=wasm` back end (`src/wasm.c`) turns LCC's intermediate
representation into a WebAssembly module — with the **LCC IR ↔ WAT opcode
correspondence** and worked, verified examples.

> Every example below is real output. Reproduce it with:
> ```sh
> build/rcc -target=bytecode foo.c     # the LCC IR (textual)
> build/rcc -target=wasm     foo.c     # the .wat module
> build/rcc -target=wasm-bin foo.c     # the binary .wasm (same module, encoded)
> ```

---

## 1. The pipeline

```
foo.c ──cpp──▶ preprocessed ──rcc front end──▶ IR forest ──wasm.c──▶ .wat text ──┐
                              (lex, parse,      (one tree per                     ├─▶ .wasm
                               types, dag)       statement)      wasmbin.c (-bin) ─┘   binary
                                                                 or wat2wasm (WABT)
```

The back end is `src/wasm.c`. It is a **tree-walker**, not an lburg grammar
(`wants_dag = 0`, no register allocation), modelled on `bytecode.c`. That works
because of one key fact:

> **LCC emits its IR in post-order — operands before operator — which is exactly
> the order a stack machine consumes them.** `a * b` is `push a; push b; mul`,
> and WebAssembly is a stack machine, so most of the back end is a one-to-one
> transcription.

---

## 2. Reading the LCC IR

Each IR node's operator name encodes three things:

```
  ADDI4
  │││└─ size in bytes (1,2,4,8)
  ││└── type:  I signed · U unsigned · F float · P pointer · V void · B struct/block
  │└─── (generic operator)
  └──── generic: ADD SUB MUL DIV MOD  BAND BOR BXOR LSH RSH NEG BCOM
                 CNST INDIR ASGN  CVI CVU CVF CVP  EQ NE LE LT GE GT
                 ADDRG ADDRL ADDRF  CALL ARG RET  JUMP LABEL
```

In `wasm.c` these are read with `generic(op)`, `optype(op)`, `opsize(op)`. The
helper `opprefix(op)` collapses (type, size) into the wasm value type:

| C type | size | `opprefix` → wasm type |
|--------|------|------------------------|
| `char`, `short`, `int`, `enum`, pointer | ≤ 4 | `i32` |
| `long long` / 64-bit | 8 | `i64` |
| `float` | 4 | `f32` |
| `double` | 8 | `f64` |

WebAssembly has only those four value types — there is no `i8`/`i16`, which is
why narrowing conversions need explicit work (see §7).

---

## 3. Opcode correspondence

### Arithmetic & bitwise — `emitexpr(l); emitexpr(r); <op>`

| LCC IR | wasm | notes |
|--------|------|-------|
| `ADD`  | `T.add` | |
| `SUB`  | `T.sub` | |
| `MUL`  | `T.mul` | |
| `DIV`  | `T.div_s` / `T.div_u` / `T.div` | signed / unsigned / float |
| `MOD`  | `T.rem_s` / `T.rem_u` | integer only |
| `BAND` | `T.and` | |
| `BOR`  | `T.or` | |
| `BXOR` | `T.xor` | |
| `LSH`  | `T.shl` | |
| `RSH`  | `T.shr_s` / `T.shr_u` | arithmetic / logical by type |
| `NEG`  | float: `T.neg`; int: `T.const 0 … T.sub` | wasm has no integer negate |
| `BCOM` | `… T.const -1  T.xor` | one's complement |

`T` = `i32`/`i64`/`f32`/`f64` from `opprefix`.

### Comparisons — `cmpsuffix(op)`

`EQ NE` → `eq ne`. For `LT LE GT GE`: float → `lt le gt ge`; signed → `lt_s le_s
gt_s ge_s`; unsigned → `lt_u le_u gt_u ge_u`. (Comparisons are emitted as part of
conditional branches — see §6.)

### Constants — `CNST`

`i32.const N` / `i64.const N` / `f32.const N` / `f64.const N`. Pointers are real
absolute addresses (fixed-base model, §8).

### Memory — `INDIR` (load) and `ASGN` (store), via `loadinstr`/`storeinstr`

| width / type | load | store |
|--------------|------|-------|
| `char`  (signed/unsigned) | `i32.load8_s` / `i32.load8_u` | `i32.store8` |
| `short` (signed/unsigned) | `i32.load16_s` / `i32.load16_u` | `i32.store16` |
| `int` / pointer | `i32.load` | `i32.store` |
| 64-bit | `i64.load` | `i64.store` |
| `float` / `double` | `f32.load` / `f64.load` | `f32.store` / `f64.store` |

A scalar local that never has its address taken skips memory entirely: `INDIR` of
such a symbol becomes a direct `local.get N` (and its store a `local.set N`).

### Conversions — `CVI CVU CVF CVP` → see §7.

### Addresses — `ADDRG ADDRL ADDRF`

| LCC IR | wasm | meaning |
|--------|------|---------|
| `ADDRGP` global data | `i32.const <addr>` | absolute data address (or a deferred `@@n@@` token, §8) |
| `ADDRGP` function | `i32.const <slot>` | the function's table index (function-pointer value) |
| `ADDRLP`/`ADDRFP` frame var | `local.get $fb` (`… i32.const off  i32.add`) | shadow-stack frame (address-taken local / struct / spilled param) |
| `ADDRFP` varargs param | `local.get $args` (`… + off`) | caller-provided arg buffer |

### Calls & return

`ARG` nodes are collected and marshalled by `emitcall`; `CALL` →
`call $name` (direct) or `call_indirect (param …)(result …)` (through a pointer);
`RET` → `return` (after restoring `$sp`).

### Control flow — `JUMP`, `LABELV`, conditional branch → §6.

---

## 4. Worked example: arithmetic + a parameter

```c
int f(int a, int b) { return a * b + 7; }
```

LCC IR (post-order) and the WAT, side by side:

```
ADDRFP4 0      local.get 0      ; a   (param 0, a scalar wasm local)
INDIRI4
ADDRFP4 4      local.get 1      ; b
INDIRI4
MULI4          i32.mul          ; a * b
CNSTI4 7       i32.const 7
ADDI4          i32.add          ; + 7
RETI4          return
```

Full module (the boilerplate is the same for every output and omitted later):

```wat
(module
  (memory 256)                                   ;; 256 * 64KiB = 16 MiB
  (export "memory" (memory 0))
  (table 0 funcref)
  (export "__indirect_function_table" (table 0))
  (global $sp (mut i32) (i32.const 16777216))     ;; shadow-stack pointer = top of memory
  (func $f (param i32) (param i32) (result i32)
    local.get 0
    local.get 1
    i32.mul
    i32.const 7
    i32.add
    return)
  (export "f" (func $f)))
```

---

## 5. Worked example: a direct call

```c
int sq(int x)   { return x * x; }
int use(void)   { return sq(5); }
```

```
;; sq                       ;; use
proc sq 4 0                 proc use 4 4
ADDRLP4 0                   CNSTI4 5        i32.const 5
ADDRFP4 0                   ARGI4           (collected, then emitted as the call's operand)
INDIRI4                     ADDRLP4 0
ASGNI4                      ADDRGP4 sq
ADDRLP4 0                   CALLI4          call $sq
INDIRI4   local.get 1       ASGNI4          local.set 0
ADDRLP4 0                   ADDRLP4 0
INDIRI4   local.get 1       INDIRI4         local.get 0
MULI4     i32.mul           RETI4           return
RETI4     return
```

```wat
(func $sq (param i32) (result i32) (local i32)
  local.get 0   local.set 1          ;; x copied into a temp
  local.get 1   local.get 1   i32.mul
  return)
(func $use (result i32) (local i32)
  i32.const 5
  call $sq
  local.set 0
  local.get 0
  return)
```

Arguments are pushed in order, then `call`. No shadow-stack traffic is needed
here because everything fits in scalar locals.

---

## 6. Control flow: the `br_table` giant-loop dispatch

WebAssembly only has **structured** control flow (`block`/`loop`/`if`), but LCC
emits **`goto`-style** IR (`LABELV` + `JUMP` + conditional jumps). The back end
bridges the gap with a *dispatch loop*:

- a pre-pass over the function numbers each basic block (`LABELV` boundary) into a
  segment index, stored in the label symbol's `x.offset`;
- the body becomes nested `block`s + a `loop` + a `br_table` keyed on a synthetic
  `$state` local;
- each block falls through to the next by default;
- a `JUMP $L` becomes `i32.const <seg>  local.set $state  br $top`;
- a conditional jump becomes `<cmp>  if  i32.const <seg>  local.set $state  br $top  end`.

```c
int sum(int n) {
    int s = 0, i;
    for (i = 1; i <= n; i++) s += i;
    return s;
}
```

The IR uses `JUMPV`, `LABELV $2/$3/$5`, and `LEI4 $2` (compare-and-branch). It
becomes:

```wat
(func $sum (param i32) (result i32)
  (local i32) (local i32) (local $state i32)
  block $exit
  loop $top
    block $S4 block $S3 block $S2 block $S1 block $S0
      local.get $state
      br_table $S0 $S1 $S2 $S3 $S4 $exit      ;; dispatch on $state
    end                                        ;; ── S0: entry
      local.get 0 local.set 0                  ;;   (s=0; i=1; goto test)
      i32.const 0 local.set 2
      i32.const 1 local.set 1
      i32.const 3 local.set $state  br $top    ;;   goto $5 (the loop test = seg 3)
    end                                        ;; ── S1: loop body  s += i
      local.get 2 local.get 1 i32.add local.set 2
    end                                        ;; ── S2: increment  i++
      local.get 1 i32.const 1 i32.add local.set 1
    end                                        ;; ── S3: test  if (i<=n) goto body
      local.get 1 local.get 0 i32.le_s
      if  i32.const 1 local.set $state  br $top  end
    end                                        ;; ── S4: return s
    local.get 2
    return
  end
  end
  unreachable                                  ;; a value fn that "falls off" is UB
)
```

The `unreachable` epilogue is only emitted for value-returning functions; `void`
functions fall off as an implicit return (and restore `$sp` first).

---

## 7. Conversions (and the sign/zero-extension rule)

WebAssembly has no `i8`/`i16`, so a value of type `char`/`short` lives in an
`i32`. Width changes therefore need explicit instructions:

| conversion | wasm |
|------------|------|
| `i32` → `i64` | `i64.extend_i32_s` / `i64.extend_i32_u` |
| `i64` → `i32` | `i32.wrap_i64` |
| → `unsigned char` / `unsigned short` | `i32.const 0xff/0xffff  i32.and` (mask) |
| → `signed char` / `signed short` | `i32.const N  i32.shl  i32.const N  i32.shr_s` (sign-extend) |
| `int` → `float` | `Tf.convert_Ti_s/u` |
| `float` → `int` | `Ti.trunc_Tf_s/u` |
| `f32` ↔ `f64` | `f32.demote_f64` / `f64.promote_f32` |

The mask/sign-extend cases are essential: without them `(unsigned char)x` would
keep `x`'s high bits, so a byte like `0xa8` (loaded sign-extended as `0xffffffa8`)
would survive as a 32-bit negative — corrupting, for instance, constant data
bytes ≥ 128. (Sign-extension is done with shifts rather than `i32.extend8_s` so
the binary back end needs no extra opcode.)

```c
int uc(char c) { return (unsigned char)c; }
```

```wat
(func $uc (param i32) (result i32)
  local.get 0
  i32.const 24  i32.shl  i32.const 24  i32.shr_s   ;; (char)param  -> canonical signed byte
  local.set 0
  local.get 0
  i32.const 255  i32.and                           ;; (unsigned char) -> mask low 8 bits
  return)
```

---

## 8. Runtime model: memory, data, the table

```
0x0000000 ┌────────────┐ 0..15  reserved (a null pointer reads/writes here)
          │  globals   │ data + BSS, packed from address 16 upward
0x0080000 │  literals  │ strings & compiler-generated constants (LITBASE)
          │    ...     │
0x0200000 │  heap      │ malloc bump arena (in the wasm libc)
          │    ↕       │
0x1000000 └────────────┘ 16 MiB = STACKTOP; $sp shadow stack grows downward
```

- **`(memory 256)`** — 256 pages = 16 MiB of linear memory, exported so the host
  (or the next instance) can read it.
- **`(global $sp …)`** — the shadow stack pointer, initialised to the top. A
  function with address-taken locals / structs / spilled params subtracts its
  frame size from `$sp` in the prologue and restores it on every exit.
- **`(data …)`** segments — globals with initialisers, and the literal pool. Two
  regions are kept apart (`dataoff` from 16, `litoff` from `0x80000`) so a string
  referenced *while another global's bytes are being emitted* can't overlap it.
- **`(table funcref)` + `(elem …)`** — populated only when a function's address is
  taken; `call_indirect` indexes it.
- **imports** — only *functions* are imported (`(import "env" "name" …)`); memory,
  table and globals are defined locally. The C library reaches the outside world
  through ~3 host calls (`__read`/`__write`/`__exit`).

```c
int   g[4];
char *msg = "hi";
int   get(int i) { return g[i]; }
```

```wat
(func $get (param i32) (result i32)
  local.get 0  i32.const 2  i32.shl   ;; i << 2  (sizeof(int))
  i32.const 20  i32.add               ;; + &g
  i32.load
  return)
(data (i32.const 16)     "\00\00\08\00")  ;; msg @16 = 0x00080000 = &"hi"
(data (i32.const 524288) "\68\69\00")     ;; "hi\0" in the literal pool (0x80000)
```

`msg` (a pointer, 4 bytes) is placed at 16; `g` (16-byte BSS array) follows at 20;
the string `"hi"` goes to the literal pool at `0x80000`, and `msg`'s initialiser
is its address. Because globals can be *referenced before they are placed*, the
emitter writes a `@@n@@` placeholder for such an address and back-patches it from
the symbol's own byte-counted address at `progend` (this is what makes
`switch` jump tables and `&global` initialisers come out right).

---

## 9. Text → binary (`-target=wasm-bin`)

`-target=wasm` emits `.wat` text; `-target=wasm-bin` runs that through an
in-process assembler (`src/wasmbin.c`) and writes a binary `.wasm` directly — no
external `wat2wasm`/`wabt` needed. The two outputs are semantically identical; the
binary path is what lets the in-browser self-host ship as a single ~280 KB module
with no assembler dependency.

The assembler consumes the closed, flat (non-folded) instruction subset the
emitter produces and encodes the standard MVP sections (type/import/function/
table/memory/global/export/element/code/data), resolving branch label depths over
a `block`/`loop`/`if` scope stack and interning `call_indirect` signatures.

If compilation hits an error (`errcnt > 0`), **no module is written** and the
process exits non-zero (via `exit()` so the self-host's host sees the status) —
so a broken compile fails a build instead of silently shipping a trapping module.

---

## 10. Map of the back end (`src/wasm.c`)

| area | functions |
|------|-----------|
| value codegen | `emitexpr` (the big opcode switch), `opprefix`, `loadinstr`, `storeinstr`, `cmpsuffix` |
| addresses | `emitaddr`, `emitba` (struct/block address), `gaddr` |
| statements / control flow | `emitroot`, the `function` hook (pre-pass + dispatch), `JUMP`/branch/`RET` |
| calls | `emitcall`, the deferred `ARG` buffer |
| data & layout | `global`, `defconst`, `defstring`, `space`, `emitbyte`, `flushdata`, the `coderefs`/`@@n@@` back-patch |
| module assembly | `progbeg`, `progend` (stitches imports/funcs/data into module order) |
| binary | `src/wasmbin.c` |

See also [`examples/hello-world`](../examples/hello-world) (C → wasm → run) and
[`examples/self-host`](../examples/self-host) (the compiler compiled to wasm,
compiling C in Node and the browser).
