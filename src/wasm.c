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

/* wasm local index stashed in sym->x.offset as (index+1); 0 == unassigned */
#define WIDX(sym) ((sym)->x.offset - 1)
#define HASWIDX(sym) ((sym)->x.offset != 0)

/* ---- type mapping ---- */
static char *wasmtype(Type ty) {
	ty = unqual(ty);
	if (isfloat(ty))
		return ty->size <= 4 ? "f32" : "f64";
	return ty->size <= 4 ? "i32" : "i64";   /* int/unsigned/pointer/enum */
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

static void emitexpr(Node p);

/* emit "call $name" for a direct CALL; args already pushed by ARG roots */
static void emitcall(Node p) {
	Node f = p->kids[0];
	if (f && generic(f->op) == ADDRG) {
		bfmt(&funcs, "call $%s\n", f->syms[0]->name);
	} else {
		/* indirect call -> M3 (needs table + type) */
		bfmt(&funcs, ";; UNSUPPORTED indirect CALL\n");
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
		if (l && (generic(l->op) == ADDRL || generic(l->op) == ADDRF) && HASWIDX(l->syms[0])) {
			if (l->syms[0]->addressed)
				bfmt(&funcs, ";; UNSUPPORTED address-taken local (needs shadow stack, M3)\n");
			bfmt(&funcs, "local.get %d\n", WIDX(l->syms[0]));
			return;
		}
		bfmt(&funcs, ";; UNSUPPORTED INDIR (memory load, M3)\n");
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
		bfmt(&funcs, ";; UNSUPPORTED address-of (memory, M3)\n");
		return;
	}
	bfmt(&funcs, ";; UNSUPPORTED expr op=%s\n", opname(op));
}

/* emit one statement-level forest root */
static void emitroot(Node p) {
	switch (generic(p->op)) {
	case ASGN: {
		Node dst = p->kids[0];
		if (dst && (generic(dst->op) == ADDRL || generic(dst->op) == ADDRF) && HASWIDX(dst->syms[0])
		    && !dst->syms[0]->addressed && optype(p->op) != B) {
			emitexpr(p->kids[1]);
			bfmt(&funcs, "local.set %d\n", WIDX(dst->syms[0]));
		} else {
			bfmt(&funcs, ";; UNSUPPORTED ASGN to memory/struct (M3)\n");
		}
		return;
	}
	case RET:
		if (p->kids[0]) emitexpr(p->kids[0]);
		bfmt(&funcs, "return\n");
		return;
	case ARG:
		emitexpr(p->kids[0]);   /* leave on stack for the upcoming CALL */
		return;
	case CALL:
		emitcall(p);
		if (optype(p->op) != V) bfmt(&funcs, "drop\n");   /* discarded result */
		return;
	case LABEL:
		/* segment boundary: close the block for the segment we're entering */
		if (usedispatch) bput(&funcs, "end\n");
		return;
	case JUMP: {
		Node a = p->kids[0];
		if (usedispatch && a && generic(a->op) == ADDRG)
			bfmt(&funcs, "i32.const %d\nlocal.set $state\nbr $top\n", a->syms[0]->x.offset);
		else
			bfmt(&funcs, ";; UNSUPPORTED indirect JUMP (switch table, M3)\n");
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
	if (nextlocal >= MAXLOCALS) { bfmt(&funcs, ";; too many locals\n"); return; }
	p->x.offset = nextlocal + 1;                 /* wasm local index = nextlocal */
	if (nlocaltypes < MAXLOCALS)
		localtype[nlocaltypes++] = wasmtype(p->type);
	nextlocal++;
}

static void I(function)(Symbol f, Symbol caller[], Symbol callee[], int ncalls) {
	int i;
	Type rty;
	Code cp;
	Node p;

	nextlocal = 0;
	nlocaltypes = 0;
	for (i = 0; caller[i] && callee[i]; i++) {
		caller[i]->x.offset = callee[i]->x.offset = i + 1;   /* params get indices 0..n-1 */
		nextlocal = i + 1;
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

	/* function header */
	bfmt(&funcs, "(func $%s", f->name);
	for (i = 0; caller[i]; i++)
		bfmt(&funcs, " (param %s)", wasmtype(caller[i]->type));
	rty = freturn(f->type);
	if (unqual(rty)->op != VOID)
		bfmt(&funcs, " (result %s)", wasmtype(rty));
	bput(&funcs, "\n");
	/* local declarations (after params in the index space) */
	for (i = 0; i < nlocaltypes; i++)
		bfmt(&funcs, "  (local %s)\n", localtype[i]);
	if (usedispatch)
		bput(&funcs, "  (local $state i32)\n");

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
	bfmt(&exports, "(export \"%s\" (func $%s))\n", p->name, p->name);
}

static void I(import)(Symbol p) {
	Type ty = p->type;
	if (!isfunc(ty)) { bfmt(&imports, ";; UNSUPPORTED data import %s (M3)\n", p->name); return; }
	bfmt(&imports, "(import \"env\" \"%s\" (func $%s", p->name, p->name);
	if (ty->u.f.proto) {
		int i;
		for (i = 0; ty->u.f.proto[i]; i++)
			if (unqual(ty->u.f.proto[i])->op != VOID)
				bfmt(&imports, " (param %s)", wasmtype(ty->u.f.proto[i]));
	}
	{
		Type rty = freturn(ty);
		if (unqual(rty)->op != VOID)
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

static void I(progbeg)(int argc, char *argv[]) {
	imports.p = funcs.p = exports.p = 0;
	imports.len = funcs.len = exports.len = 0;
	imports.cap = funcs.cap = exports.cap = 0;
}

static void I(progend)(void) {
	print("(module\n");
	if (imports.p) print("%s", imports.p);
	if (funcs.p)   print("%s", funcs.p);
	if (exports.p) print("%s", exports.p);
	print(")\n");
}

/* ---- minimal / deferred hooks ---- */
static void I(address)(Symbol q, Symbol p, long n) { q->x.name = stringf("%s%s%D", p->x.name, n > 0 ? "+" : "", n); }
static void I(blockbeg)(Env *e) {}
static void I(blockend)(Env *e) {}
static void I(defaddress)(Symbol p) {}     /* M3: data segments */
static void I(defconst)(int suffix, int size, Value v) {}
static void I(defstring)(int len, char *s) {}
static void I(global)(Symbol p) {}
static void I(segment)(int s) {}
static void I(space)(int n) {}

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
