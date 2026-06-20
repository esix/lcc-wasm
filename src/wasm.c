/* wasm.c - WebAssembly (.wat text) back end for lcc.
 *
 * Emits a WebAssembly module in text format (.wat), to be assembled to binary
 * with wat2wasm (WABT). Modeled on bytecode.c: wants_dag=0, no lburg, no
 * register allocation -- a tree-walking emitter, since wasm is a stack machine
 * and lcc's IR is already emitted in post-order (operands before operator).
 *
 * MILESTONE M1 (straight-line):
 *   - scalar params/locals (not address-taken) -> wasm typed locals
 *   - i32/i64/f32/f64 arithmetic, bitwise, shift, neg, bcom, const
 *   - direct calls (env imports), return
 * DEFERRED: control flow (M2, br_table dispatch), memory/shadow-stack/globals
 *   (M3), structs/varargs/conversions polish (M4). Unsupported nodes emit a
 *   ";; UNSUPPORTED ..." marker so the .wat is inspectable.
 */
#include "c.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define I(f) wasm_##f

/* ---- growable output buffers (assembled into module order at progend) ---- */
typedef struct { char *p; int len, cap; } Buf;
static Buf imports, funcs, exports;

static void bput(Buf *b, const char *s) {
	int n = (int)strlen(s);
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->cap + n + 1) * 2;
		b->p = realloc(b->p, b->cap);
		assert(b->p);
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void bfmt(Buf *b, const char *fmt, ...) {
	char tmp[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof tmp, fmt, ap);
	va_end(ap);
	bput(b, tmp);
}

/* ---- per-function state ---- */
static int nextlocal;            /* next wasm local index to assign */
#define MAXLOCALS 1024
static char *localtype[MAXLOCALS]; /* wasm type of declared (non-param) locals */
static int nlocaltypes;

/* per-symbol storage classification, tagged in x.usecount; the value (wasm
   local index / shadow-stack frame offset / absolute data address) is in x.offset */
/* K_ARGBUF: parameter of a variadic function, living in the caller-provided
   shadow-stack arg buffer at offset SVAL, addressed via local $args. */
enum { K_NONE = 0, K_WLOCAL, K_FRAME, K_DATA, K_FUNC, K_ARGBUF };
#define SKIND(s) ((s)->x.usecount)
#define SVAL(s)  ((s)->x.offset)
#define VASLOT 8                  /* every marshalled arg occupies an 8-byte slot */

/* ---- indirect function table (for function pointers / call_indirect) ---- */
#define MAXFUNCS 4096
static char *tablefn[MAXFUNCS];   /* function names, indexed by table slot */
static int ntable;
static Node argvec[64];           /* ARG nodes collected for the pending call (emission deferred) */
static int argc;

/* ---- type mapping ---- */
static char *wasmtype(Type ty) {
	ty = unqual(ty);
	if (isfloat(ty))
		return ty->size <= 4 ? "f32" : "f64";
	return ty->size <= 4 ? "i32" : "i64";   /* int/unsigned/pointer/enum */
}

/* parameter type: structs/unions are passed by pointer (wants_argb=0) */
static char *paramtype(Type ty) {
	return isstruct(ty) ? "i32" : wasmtype(ty);
}

/* wasm type prefix for an arithmetic opcode, from its type-class + size */
static char *opprefix(int op) {
	int sz = opsize(op);
	if (optype(op) == F)
		return sz <= 4 ? "f32" : "f64";
	return sz <= 4 ? "i32" : "i64";
}

/* wasm compare suffix for a comparison opcode (signedness from operand type) */
static char *cmpsuffix(int op) {
	int t = optype(op);
	switch (generic(op)) {
	case EQ: return "eq";
	case NE: return "ne";
	case LT: return t == F ? "lt" : t == U ? "lt_u" : "lt_s";
	case LE: return t == F ? "le" : t == U ? "le_u" : "le_s";
	case GT: return t == F ? "gt" : t == U ? "gt_u" : "gt_s";
	case GE: return t == F ? "ge" : t == U ? "ge_u" : "ge_s";
	}
	return "eq";
}

/* control-flow state for the current function */
static int usedispatch;   /* function has goto-style control flow -> giant-loop dispatch */
static int segcount;      /* number of basic-block segments (entry + each label) */

/* ---- linear memory / data state ---- */
#define MEMPAGES 256                 /* 256 * 64KiB = 16 MiB */
#define STACKTOP (MEMPAGES * 65536)
static Buf data;                     /* accumulated (data ...) segments */
static Buf pending;                  /* raw bytes of the current data segment */
static int pendaddr;                 /* start address of the pending segment */
static int dataoff = 16;             /* next free data address (0..15 reserved/NULL) */
static int curseg;                   /* current CODE/DATA/BSS/LIT segment */
static int framebytes;                /* per-function shadow-stack frame bytes */

/* append one raw byte to the pending data segment (handles NUL) */
static void praw(int c) {
	if (pending.len + 1 > pending.cap) {
		pending.cap = (pending.cap + 1) * 2;
		pending.p = realloc(pending.p, pending.cap);
		assert(pending.p);
	}
	pending.p[pending.len++] = (char)c;
}

/* does symbol s live in linear memory (vs being a wasm local)? */
static int inmem(Symbol s) {
	return s->addressed || !(isarith(s->type) || isptr(s->type));
}

/* assign/return a data symbol's absolute address (lazy -> handles forward refs) */
static int gaddr(Symbol s) {
	if (SKIND(s) != K_DATA) {
		int a = s->type->align ? s->type->align : 1;
		dataoff = roundup(dataoff, a);
		SVAL(s) = dataoff;
		SKIND(s) = K_DATA;
		dataoff += s->type->size > 0 ? s->type->size : a;
	}
	return SVAL(s);
}

static char *loadinstr(int op) {
	int sz = opsize(op), u = optype(op) == U;
	if (optype(op) == F) return sz == 4 ? "f32.load" : "f64.load";
	switch (sz) {
	case 1:  return u ? "i32.load8_u"  : "i32.load8_s";
	case 2:  return u ? "i32.load16_u" : "i32.load16_s";
	case 8:  return "i64.load";
	default: return "i32.load";
	}
}

static char *storeinstr(int op) {
	int sz = opsize(op);
	if (optype(op) == F) return sz == 4 ? "f32.store" : "f64.store";
	switch (sz) {
	case 1:  return "i32.store8";
	case 2:  return "i32.store16";
	case 8:  return "i64.store";
	default: return "i32.store";
	}
}

/* assign/return a function's table slot (its function-pointer value) */
static int funcindex(Symbol s) {
	if (SKIND(s) != K_FUNC) {
		SKIND(s) = K_FUNC;
		SVAL(s) = ntable;
		if (ntable < MAXFUNCS) tablefn[ntable] = s->name;
		ntable++;
	}
	return SVAL(s);
}

static void emitexpr(Node p);
static void emitaddr(Node p);

/* emit a CALL. Args were collected (not emitted) by the preceding ARG roots so
   that a variadic callee can marshal them into a shadow-stack buffer. */
static void emitcall(Node p) {
	Node f = p->kids[0];
	Node args[64];
	int n = argc, i, direct;
	Symbol fn = (f && generic(f->op) == ADDRG && isfunc(f->syms[0]->type)) ? f->syms[0] : NULL;

	for (i = 0; i < n; i++) args[i] = argvec[i];   /* snapshot; reset so nested calls don't clobber */
	argc = 0;
	direct = fn != NULL;

	if (direct && variadic(fn->type)) {
		/* marshal every arg into an 8-byte-slot buffer on the shadow stack;
		   pass a pointer to it as the callee's single argument. */
		int bufsize = roundup(n * VASLOT, 16);
		bfmt(&funcs, "global.get $sp\ni32.const %d\ni32.sub\nglobal.set $sp\n", bufsize);
		for (i = 0; i < n; i++) {
			bput(&funcs, "global.get $sp\n");
			emitexpr(args[i]->kids[0]);
			bfmt(&funcs, "%s.store offset=%d\n", opprefix(args[i]->op), i * VASLOT);
		}
		bput(&funcs, "global.get $sp\n");                 /* arg = buffer base */
		bfmt(&funcs, "call $%s\n", fn->name);
		bfmt(&funcs, "global.get $sp\ni32.const %d\ni32.add\nglobal.set $sp\n", bufsize);
		return;
	}

	for (i = 0; i < n; i++) emitexpr(args[i]->kids[0]);   /* push args */
	if (direct) {
		bfmt(&funcs, "call $%s\n", fn->name);
	} else {
		emitexpr(f);                                       /* push function-pointer (table index) */
		bput(&funcs, "call_indirect");
		for (i = 0; i < n; i++) bfmt(&funcs, " (param %s)", opprefix(args[i]->op));
		if (optype(p->op) != V && optype(p->op) != B) bfmt(&funcs, " (result %s)", opprefix(p->op));
		bput(&funcs, "\n");
	}
}

/* push the value of expression tree p onto the wasm operand stack */
static void emitexpr(Node p) {
	int op = p->op;
	Node l = p->kids[0], r = p->kids[1];

	switch (generic(op)) {
	case CNST:
		switch (optype(op)) {
		case I: bfmt(&funcs, "%s.const %ld\n", opprefix(op), (long)p->syms[0]->u.c.v.i); return;
		case U: bfmt(&funcs, "%s.const %lu\n", opprefix(op), (unsigned long)p->syms[0]->u.c.v.u); return;
		case P: bfmt(&funcs, "i32.const %lu\n", (unsigned long)(size_t)p->syms[0]->u.c.v.p); return; /* M3: real addr */
		case F: bfmt(&funcs, "%s.const %.17g\n", opprefix(op), (double)p->syms[0]->u.c.v.d); return;
		}
		break;
	case INDIR:
		if (l && (generic(l->op) == ADDRL || generic(l->op) == ADDRF) && SKIND(l->syms[0]) == K_WLOCAL) {
			bfmt(&funcs, "local.get %d\n", SVAL(l->syms[0]));   /* scalar wasm-local fast path */
		} else {
			emitaddr(l);                                        /* push address */
			bfmt(&funcs, "%s\n", loadinstr(op));
		}
		return;
	case ADD: emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.add\n", opprefix(op)); return;
	case SUB: emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.sub\n", opprefix(op)); return;
	case MUL: emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.mul\n", opprefix(op)); return;
	case DIV: emitexpr(l); emitexpr(r);
		bfmt(&funcs, optype(op) == F ? "%s.div\n" : optype(op) == U ? "%s.div_u\n" : "%s.div_s\n", opprefix(op));
		return;
	case MOD: emitexpr(l); emitexpr(r);
		bfmt(&funcs, optype(op) == U ? "%s.rem_u\n" : "%s.rem_s\n", opprefix(op));
		return;
	case BAND: emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.and\n", opprefix(op)); return;
	case BOR:  emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.or\n",  opprefix(op)); return;
	case BXOR: emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.xor\n", opprefix(op)); return;
	case LSH:  emitexpr(l); emitexpr(r); bfmt(&funcs, "%s.shl\n", opprefix(op)); return;
	case RSH:  emitexpr(l); emitexpr(r);
		bfmt(&funcs, optype(op) == U ? "%s.shr_u\n" : "%s.shr_s\n", opprefix(op));
		return;
	case NEG:
		if (optype(op) == F) { emitexpr(l); bfmt(&funcs, "%s.neg\n", opprefix(op)); }
		else { bfmt(&funcs, "%s.const 0\n", opprefix(op)); emitexpr(l); bfmt(&funcs, "%s.sub\n", opprefix(op)); }
		return;
	case BCOM:
		emitexpr(l); bfmt(&funcs, "%s.const -1\n", opprefix(op)); bfmt(&funcs, "%s.xor\n", opprefix(op));
		return;
	case CALL:
		emitcall(p);
		return;
	case CVI: case CVU: case CVP: case CVF: {
		/* generic = source class, node type/size = target; syms[0] = source size */
		char *src, *dst = opprefix(op);
		int srcsz = p->syms[0] ? (int)p->syms[0]->u.c.v.i : opsize(l->op);
		int srcclass = generic(op);  /* CVI/CVU from int/unsigned; CVF from float; CVP from ptr */
		emitexpr(l);
		src = (srcclass == CVF) ? (srcsz <= 4 ? "f32" : "f64") : (srcsz <= 4 ? "i32" : "i64");
		if (strcmp(src, dst) == 0) return;                       /* same wasm type: nop */
		if (src[0] == 'i' && dst[0] == 'i') {                    /* int<->int width */
			if (dst[1] == '6') bfmt(&funcs, "i64.extend_i32_%s\n", srcclass == CVU ? "u" : "s");
			else bfmt(&funcs, "i32.wrap_i64\n");
		} else if (src[0] == 'f' && dst[0] == 'f') {
			bfmt(&funcs, dst[1] == '3' ? "f32.demote_f64\n" : "f64.promote_f32\n");
		} else if (src[0] == 'i' && dst[0] == 'f') {             /* int -> float */
			bfmt(&funcs, "%s.convert_%s_%s\n", dst, src, srcclass == CVU ? "u" : "s");
		} else {                                                 /* float -> int */
			bfmt(&funcs, "%s.trunc_%s_%s\n", dst, src, optype(op) == U ? "u" : "s");
		}
		return;
	}
	case ADDRG: case ADDRL: case ADDRF:
		emitaddr(p);
		return;
	}
	bfmt(&funcs, ";; UNSUPPORTED expr op=%s\n", opname(op));
}

/* push the ADDRESS of an lvalue/symbol */
static void emitaddr(Node p) {
	Symbol s;
	switch (generic(p->op)) {
	case ADDRG:
		s = p->syms[0];
		if (isfunc(s->type))   /* function pointer value = its table slot */
			bfmt(&funcs, "i32.const %d\n", funcindex(s));
		else
			bfmt(&funcs, "i32.const %d\n", gaddr(s));
		return;
	case ADDRL: case ADDRF:
		s = p->syms[0];
		if (SKIND(s) == K_FRAME) {
			if (SVAL(s) == 0) bput(&funcs, "local.get $fb\n");
			else bfmt(&funcs, "local.get $fb\ni32.const %d\ni32.add\n", SVAL(s));
		} else if (SKIND(s) == K_ARGBUF) {              /* variadic-function param in the arg buffer */
			if (SVAL(s) == 0) bput(&funcs, "local.get $args\n");
			else bfmt(&funcs, "local.get $args\ni32.const %d\ni32.add\n", SVAL(s));
		} else {
			bfmt(&funcs, ";; UNSUPPORTED &wasm-local (expected addressed)\ni32.const 0\n");
		}
		return;
	default:
		emitexpr(p);   /* general address expression (e.g. a pointer value) */
		return;
	}
}

/* push the ADDRESS of a struct (B-type) operand: a struct rvalue is INDIRB(addr),
   so unwrap the INDIR and evaluate its address operand */
static void emitba(Node n) {
	if (n && generic(n->op) == INDIR && optype(n->op) == B) emitexpr(n->kids[0]);
	else emitexpr(n);   /* scalar INDIR (e.g. *dp) already yields the address as its value */
}

/* emit one statement-level forest root */
static void emitroot(Node p) {
	switch (generic(p->op)) {
	case ASGN: {
		Node dst = p->kids[0];
		if (optype(p->op) == B) {                               /* struct copy -> memory.copy */
			emitba(p->kids[0]);                                 /* dest address */
			emitba(p->kids[1]);                                 /* src address */
			bfmt(&funcs, "i32.const %lu\nmemory.copy\n", (unsigned long)p->syms[0]->u.c.v.u);
		} else if (dst && (generic(dst->op) == ADDRL || generic(dst->op) == ADDRF) && SKIND(dst->syms[0]) == K_WLOCAL) {
			emitexpr(p->kids[1]);                               /* scalar wasm-local store */
			bfmt(&funcs, "local.set %d\n", SVAL(dst->syms[0]));
		} else {
			emitaddr(dst);                                      /* address */
			emitexpr(p->kids[1]);                               /* value */
			bfmt(&funcs, "%s\n", storeinstr(p->op));
		}
		return;
	}
	case RET:
		if (p->kids[0]) emitexpr(p->kids[0]);
		if (framebytes > 0)
			bfmt(&funcs, "local.get $fb\ni32.const %d\ni32.add\nglobal.set $sp\n", framebytes);
		bfmt(&funcs, "return\n");
		return;
	case ARG:
		if (argc < 64) argvec[argc++] = p;   /* defer; emitted by emitcall (per-callee ABI) */
		return;
	case CALL:
		emitcall(p);
		if (optype(p->op) != V && optype(p->op) != B) bfmt(&funcs, "drop\n");   /* discarded scalar result */
		return;
	case LABEL:
		/* segment boundary: close the block for the segment we're entering */
		if (usedispatch) bput(&funcs, "end\n");
		return;
	case JUMP: {
		Node a = p->kids[0];
		if (!usedispatch) return;
		if (a && generic(a->op) == ADDRG) {           /* direct: goto label */
			bfmt(&funcs, "i32.const %d\nlocal.set $state\nbr $top\n", a->syms[0]->x.offset);
		} else {                                       /* indirect: switch jump-table dispatch */
			emitexpr(a);                               /* loads the target segment index from the table */
			bput(&funcs, "local.set $state\nbr $top\n");
		}
		return;
	}
	case EQ: case NE: case LT: case LE: case GT: case GE:
		/* compare-and-branch: if (k0 <cmp> k1) goto syms[0] */
		if (usedispatch) {
			emitexpr(p->kids[0]);
			emitexpr(p->kids[1]);
			bfmt(&funcs, "%s.%s\n", opprefix(p->op), cmpsuffix(p->op));
			bfmt(&funcs, "if\ni32.const %d\nlocal.set $state\nbr $top\nend\n", p->syms[0]->x.offset);
		} else {
			bfmt(&funcs, ";; UNSUPPORTED compare-branch without dispatch\n");
		}
		return;
	}
	bfmt(&funcs, ";; UNSUPPORTED stmt op=%s\n", opname(p->op));
}

/* ===================== Interface hooks ===================== */

static char rcsid[] = "$Id: wasm back end, milestone M1 $";

static Node I(gen)(Node p) { return p; }   /* no instruction selection; emit walks trees */

static void I(emit)(Node forest) {
	Node p;
	for (p = forest; p; p = p->link)
		emitroot(p);
}

static void I(local)(Symbol p) {
	if (inmem(p)) {                              /* address-taken or aggregate -> shadow stack */
		int a = p->type->align ? p->type->align : 1;
		framebytes = roundup(framebytes, a);
		SVAL(p) = framebytes;                 /* frame offset (may be 0) */
		SKIND(p) = K_FRAME;
		framebytes += p->type->size;
		return;
	}
	if (nextlocal >= MAXLOCALS) { bfmt(&funcs, ";; too many locals\n"); return; }
	SVAL(p) = nextlocal;                         /* wasm local index */
	SKIND(p) = K_WLOCAL;
	if (nlocaltypes < MAXLOCALS)
		localtype[nlocaltypes++] = wasmtype(p->type);
	nextlocal++;
}

static void I(function)(Symbol f, Symbol caller[], Symbol callee[], int ncalls) {
	int i;
	Type rty;
	Code cp;
	Node p;

	int isva = variadic(f->type);
	nextlocal = 0;
	nlocaltypes = 0;
	framebytes = 0;
	if (isva) {
		/* one wasm param ($args = local 0); C params live in the arg buffer */
		nextlocal = 1;
		for (i = 0; caller[i] && callee[i]; i++) {
			SVAL(caller[i]) = SVAL(callee[i]) = i * VASLOT;
			SKIND(caller[i]) = SKIND(callee[i]) = K_ARGBUF;
		}
	} else {
		for (i = 0; caller[i] && callee[i]; i++) {
			SVAL(caller[i]) = SVAL(callee[i]) = i;       /* params -> wasm locals 0..n-1 */
			SKIND(caller[i]) = SKIND(callee[i]) = K_WLOCAL;
			nextlocal = i + 1;
		}
	}
	offset = maxoffset = argoffset = maxargoffset = 0;

	gencode(caller, callee);   /* triggers local() for each local/temp (indices n..) */

	/* pre-pass: number basic-block segments (entry = 0; each label = 1,2,...)
	   and detect goto-style control flow */
	segcount = 1;
	usedispatch = 0;
	for (cp = codehead.next; cp; cp = cp->next)
		if (cp->kind == Gen || cp->kind == Jump || cp->kind == Label)
			for (p = cp->u.forest; p; p = p->link) {
				int g = generic(p->op);
				if (g == LABEL) p->syms[0]->x.offset = segcount++;
				else if (g == JUMP) usedispatch = 1;
				else if (g >= EQ && g <= NE) usedispatch = 1;
			}

	/* function header (struct return -> hidden pointer param + void result) */
	bfmt(&funcs, "(func $%s", f->name);
	if (isva)
		bput(&funcs, " (param $args i32)");           /* pointer to the marshalled arg buffer */
	else
		for (i = 0; caller[i]; i++)
			bfmt(&funcs, " (param %s)", paramtype(caller[i]->type));
	rty = freturn(f->type);
	if (unqual(rty)->op != VOID && !isstruct(rty))
		bfmt(&funcs, " (result %s)", wasmtype(rty));
	bput(&funcs, "\n");
	/* a function with a compile error has no body (gencode/emitcode skip on
	   errcnt>0); emit a trivially-valid body so the module still parses */
	if (errcnt > 0) { bput(&funcs, "unreachable\n)\n"); return; }
	/* local declarations (after params in the index space) */
	for (i = 0; i < nlocaltypes; i++)
		bfmt(&funcs, "  (local %s)\n", localtype[i]);
	if (usedispatch)
		bput(&funcs, "  (local $state i32)\n");
	if (framebytes > 0) {
		framebytes = roundup(framebytes, 16);
		bput(&funcs, "  (local $fb i32)\n");
	}

	/* shadow-stack prologue: $fb = ($sp -= framebytes) */
	if (framebytes > 0)
		bfmt(&funcs, "global.get $sp\ni32.const %d\ni32.sub\nlocal.tee $fb\nglobal.set $sp\n", framebytes);

	/* dispatch scaffolding: one loop + a br_table over per-segment blocks */
	if (usedispatch) {
		bput(&funcs, "block $exit\nloop $top\n");
		for (i = segcount - 1; i >= 0; i--)
			bfmt(&funcs, "block $S%d\n", i);
		bput(&funcs, "local.get $state\nbr_table");
		for (i = 0; i < segcount; i++)
			bfmt(&funcs, " $S%d", i);
		bput(&funcs, " $exit\n");
		bput(&funcs, "end\n");   /* closes $S0; entry segment (state 0) falls through here */
	}

	emitcode();                /* emits body; LABEL/JUMP/compare handled in emitroot */

	if (usedispatch)
		bput(&funcs, "end\nend\nunreachable\n");   /* close loop $top, block $exit */
	bput(&funcs, ")\n");
}

static void I(export)(Symbol p) {
	if (isfunc(p->type))   /* data globals live in memory; nothing to export in the wasm sense */
		bfmt(&exports, "(export \"%s\" (func $%s))\n", p->name, p->name);
}

static void I(import)(Symbol p) {
	Type ty = p->type;
	if (!isfunc(ty)) { bfmt(&imports, ";; UNSUPPORTED data import %s (M3)\n", p->name); return; }
	bfmt(&imports, "(import \"env\" \"%s\" (func $%s", p->name, p->name);
	if (variadic(ty)) {
		bput(&imports, " (param i32)");   /* single pointer to the marshalled arg buffer */
	} else if (ty->u.f.proto) {
		int i;
		for (i = 0; ty->u.f.proto[i]; i++)
			if (unqual(ty->u.f.proto[i])->op != VOID)
				bfmt(&imports, " (param %s)", paramtype(ty->u.f.proto[i]));
	}
	{
		Type rty = freturn(ty);
		if (unqual(rty)->op != VOID && !isstruct(rty))
			bfmt(&imports, " (result %s)", wasmtype(rty));
	}
	bput(&imports, "))\n");
}

static void I(defsymbol)(Symbol p) {
	if (p->scope == CONSTANTS)
		switch (optype(ttob(p->type))) {
		case I: p->x.name = stringf("%D", p->u.c.v.i); break;
		case U: p->x.name = stringf("%U", p->u.c.v.u); break;
		case P: p->x.name = stringf("%U", p->u.c.v.p); break;
		default: p->x.name = p->name; break;
		}
	else if (p->generated)
		p->x.name = stringf("$%s", p->name);
	else
		p->x.name = p->name;
}

/* flush the pending data segment as a (data (i32.const A) "..") directive */
static void flushdata(void) {
	int i;
	if (pending.len <= 0) return;
	bfmt(&data, "(data (i32.const %d) \"", pendaddr);
	for (i = 0; i < pending.len; i++)
		bfmt(&data, "\\%02x", (unsigned char)pending.p[i]);
	bput(&data, "\")\n");
	pending.len = 0;
}

static void I(progbeg)(int argc, char *argv[]) {
	imports.p = funcs.p = exports.p = data.p = 0;
	imports.len = funcs.len = exports.len = data.len = 0;
	imports.cap = funcs.cap = exports.cap = data.cap = 0;
	pending.p = 0; pending.len = pending.cap = 0;
	dataoff = 16; curseg = 0;
	ntable = 0; argc = 0;
}

static void I(progend)(void) {
	int i;
	flushdata();
	print("(module\n");
	if (imports.p) print("%s", imports.p);
	print("(memory %d)\n", MEMPAGES);
	print("(export \"memory\" (memory 0))\n");
	/* always declare the table so a call_indirect validates even in a unit that
	   takes no function address locally (the elem is populated only if ntable>0) */
	print("(table %d funcref)\n", ntable);
	print("(export \"__indirect_function_table\" (table 0))\n");
	print("(global $sp (mut i32) (i32.const %d))\n", STACKTOP);
	if (funcs.p)   print("%s", funcs.p);
	if (exports.p) print("%s", exports.p);
	if (ntable > 0) {
		print("(elem (i32.const 0)");
		for (i = 0; i < ntable; i++) print(" $%s", tablefn[i]);
		print(")\n");
	}
	if (data.p)    print("%s", data.p);
	print(")\n");
}

/* ---- data / segment hooks ---- */
static void I(segment)(int s) { curseg = s; }

static void I(global)(Symbol p) {
	flushdata();
	pendaddr = gaddr(p);
}

static void I(defconst)(int suffix, int size, Value v) {
	int i;
	if (suffix == F) {
		if (size == 4) { float f = (float)v.d; unsigned char *b = (unsigned char *)&f; for (i = 0; i < 4; i++) praw(b[i]); }
		else           { double d = (double)v.d; unsigned char *b = (unsigned char *)&d; for (i = 0; i < 8; i++) praw(b[i]); }
		return;
	}
	{
		unsigned long val = suffix == P ? (unsigned long)(size_t)v.p
		                  : suffix == U ? v.u : (unsigned long)v.i;
		for (i = 0; i < size; i++) praw((val >> (8 * i)) & 0xff);
	}
}

static void I(defstring)(int len, char *s) {
	int i;
	for (i = 0; i < len; i++) praw((unsigned char)s[i]);
}

static void I(space)(int n) {       /* BSS is zero by default; only pad inside initialized data */
	int i;
	if (curseg != BSS) for (i = 0; i < n; i++) praw(0);
}

static void putaddr(int a) { int i; for (i = 0; i < 4; i++) praw((a >> (8 * i)) & 0xff); }

static void I(defaddress)(Symbol p) {   /* pointer-sized datum: a symbol's address */
	if (p->scope == LABELS)   putaddr(p->x.offset);    /* switch jump-table entry = segment index */
	else if (!p->type)        putaddr(0);
	else if (isfunc(p->type)) putaddr(funcindex(p));   /* function pointer stored in data */
	else                      putaddr(gaddr(p));        /* address of a data symbol */
}

static void I(address)(Symbol q, Symbol p, long n) {
	/* q aliases p+n: inherit p's storage kind so member/element addresses route correctly */
	if (SKIND(p) == K_NONE) gaddr(p);          /* unseen base must be a data symbol */
	SKIND(q) = SKIND(p);
	SVAL(q) = SVAL(p) + (int)n;
}
static void I(blockbeg)(Env *e) {}
static void I(blockend)(Env *e) {}

Interface wasmIR = {
	1, 1, 0,	/* char */
	2, 2, 0,	/* short */
	4, 4, 0,	/* int */
	4, 4, 0,	/* long */
	8, 8, 1,	/* long long */
	4, 4, 0,	/* float */
	8, 8, 1,	/* double */
	8, 8, 1,	/* long double (mapped to f64) */
	4, 4, 0,	/* T* */
	0, 4, 0,	/* struct */
	1,		/* little_endian */
	0,		/* mulops_calls */
	0,		/* wants_callb */
	0,		/* wants_argb */
	1,		/* left_to_right */
	0,		/* wants_dag */
	0,		/* unsigned_char */
	I(address),
	I(blockbeg),
	I(blockend),
	I(defaddress),
	I(defconst),
	I(defstring),
	I(defsymbol),
	I(emit),
	I(export),
	I(function),
	I(gen),
	I(global),
	I(import),
	I(local),
	I(progbeg),
	I(progend),
	I(segment),
	I(space),
	0,		/* stabblock */
	0,		/* stabend */
	0,		/* stabfend */
	0,		/* stabinit */
	0,		/* stabline */
	0,		/* stabsym */
	0,		/* stabtype */
};
