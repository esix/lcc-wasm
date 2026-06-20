/* wasmbin.c - assemble the closed .wat text subset emitted by wasm.c into a
 * binary WebAssembly (.wasm) module.  Strategy A (see TASK-wasm-binary-backend):
 * the text emitter is unchanged; this is a self-contained text->binary pass used
 * by `rcc -target=wasm-bin`.  One implementation, used by both the native rcc and
 * the self-hosted rcc.wasm, so it is written in plain C89 within the wasm libc.
 *
 * The input is the flat, non-folded instruction text wasm.c produces (one
 * instruction per token run); this is NOT a general WAT parser.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern void fprint(FILE *, const char *, ...);

static void die(const char *msg) {
	fputs("rcc: wasm-bin: ", stderr);
	fputs(msg, stderr);
	fputs("\n", stderr);
	exit(EXIT_FAILURE);
}

/* ---- output byte buffer ---- */
typedef struct { unsigned char *p; int len, cap; } Bytes;

static void wb(Bytes *b, int c) {
	if (b->len + 1 > b->cap) {
		b->cap = b->cap ? b->cap * 2 : 256;
		b->p = realloc(b->p, b->cap);
		if (!b->p) die("out of memory");
	}
	b->p[b->len++] = (unsigned char)c;
}
static void wbn(Bytes *b, const unsigned char *s, int n) { int i; for (i = 0; i < n; i++) wb(b, s[i]); }
static void wbs(Bytes *b, const char *s) { while (*s) wb(b, (unsigned char)*s++); }

static void uleb(Bytes *b, unsigned long v) {
	do { unsigned char by = (unsigned char)(v & 0x7f); v >>= 7; if (v) by |= 0x80; wb(b, by); } while (v);
}
static void sleb(Bytes *b, long v) {
	int more = 1;
	while (more) {
		unsigned char by = (unsigned char)(v & 0x7f);
		v >>= 7;   /* arithmetic shift (signed) */
		if ((v == 0 && !(by & 0x40)) || (v == -1 && (by & 0x40))) more = 0;
		else by |= 0x80;
		wb(b, by);
	}
}
/* name: uleb(len) + raw bytes */
static void wname(Bytes *b, const char *s, int n) { uleb(b, (unsigned long)n); wbn(b, (const unsigned char *)s, n); }

/* ---- tokenizer ---- */
#define T_EOF 0
#define T_LP  1
#define T_RP  2
#define T_WORD 3
#define T_STR 4
typedef struct { int t; char *s; int n; } Tok;
static Tok *toks; static int ntoks, captoks;
static int ti;   /* parse cursor */

static void pushtok(int t, const char *s, int n) {
	Tok *tk;
	if (ntoks >= captoks) {
		captoks = captoks ? captoks * 2 : 4096;
		toks = realloc(toks, captoks * sizeof *toks);
		if (!toks) die("out of memory");
	}
	tk = &toks[ntoks++];
	tk->t = t; tk->n = n; tk->s = 0;
	if (s) {
		tk->s = malloc(n + 1);
		if (!tk->s) die("out of memory");
		memcpy(tk->s, s, n);
		tk->s[n] = 0;
	}
}

static void tokenize(const char *p) {
	for (;;) {
		for (;;) {
			while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
			if (p[0] == ';' && p[1] == ';') { while (*p && *p != '\n') p++; continue; }
			break;
		}
		if (!*p) break;
		if (*p == '(') { pushtok(T_LP, 0, 0); p++; continue; }
		if (*p == ')') { pushtok(T_RP, 0, 0); p++; continue; }
		if (*p == '"') {
			const char *q = ++p;
			while (*p && *p != '"') { if (*p == '\\' && p[1]) p += 2; else p++; }
			pushtok(T_STR, q, (int)(p - q));
			if (*p == '"') p++;
			continue;
		}
		{
			const char *q = p;
			while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r'
			       && *p != '(' && *p != ')' && *p != '"') p++;
			pushtok(T_WORD, q, (int)(p - q));
		}
	}
}

static Tok *cur(void) { return &toks[ti]; }
static int isword(const char *s) { return ti < ntoks && toks[ti].t == T_WORD && strcmp(toks[ti].s, s) == 0; }
static char *takeword(void) {
	if (ti >= ntoks || toks[ti].t != T_WORD) die("expected word");
	return toks[ti++].s;
}
static char *takestr(int *n) {
	if (ti >= ntoks || toks[ti].t != T_STR) die("expected string");
	*n = toks[ti].n; return toks[ti++].s;
}
static void takelp(void) { if (ti >= ntoks || toks[ti].t != T_LP) die("expected ("); ti++; }
static void takerp(void) { if (ti >= ntoks || toks[ti].t != T_RP) die("expected )"); ti++; }

/* ---- small numeric parsers ---- */
static unsigned long parseu(const char *s) {
	unsigned long v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long)(*s++ - '0');
	return v;
}
static long i32val(const char *w) {
	int neg = 0; unsigned long m; unsigned int pat;
	if (*w == '-') { neg = 1; w++; }
	m = parseu(w);
	pat = neg ? (unsigned int)(0u - (unsigned int)m) : (unsigned int)m;
	return (long)(int)pat;
}
static long i64val(const char *w) {
	int neg = 0; unsigned long m;
	if (*w == '-') { neg = 1; w++; }
	m = parseu(w);
	return neg ? -(long)m : (long)m;
}
static void f32bytes(Bytes *b, const char *w) {
	union { float f; unsigned char c[4]; } u; int i;
	u.f = (float)strtod(w, (char **)0);
	for (i = 0; i < 4; i++) wb(b, u.c[i]);   /* little-endian host (native + wasm) */
}
static void f64bytes(Bytes *b, const char *w) {
	union { double d; unsigned char c[8]; } u; int i;
	u.d = strtod(w, (char **)0);
	for (i = 0; i < 8; i++) wb(b, u.c[i]);
}

/* valtype byte */
static int vt(const char *s) {
	if (!strcmp(s, "i32")) return 0x7f;
	if (!strcmp(s, "i64")) return 0x7e;
	if (!strcmp(s, "f32")) return 0x7d;
	if (!strcmp(s, "f64")) return 0x7c;
	die("bad valtype");
	return 0x7f;
}

/* ---- function-type table (dedup) ---- */
#define MAXTYPES 8192
#define MAXPARAMS 64
typedef struct { int np; unsigned char pt[MAXPARAMS]; int nr; unsigned char rt; } Sig;
static Sig tysig[MAXTYPES]; static int ntysig;
static int interntype(Sig *s) {
	int i, j;
	for (i = 0; i < ntysig; i++) {
		if (tysig[i].np != s->np || tysig[i].nr != s->nr) continue;
		if (s->nr && tysig[i].rt != s->rt) continue;
		for (j = 0; j < s->np; j++) if (tysig[i].pt[j] != s->pt[j]) break;
		if (j == s->np) return i;
	}
	if (ntysig >= MAXTYPES) die("too many tysig");
	tysig[ntysig] = *s;
	return ntysig++;
}

/* ---- index spaces ---- */
#define MAXFUNCS 16384
static char *fnames[MAXFUNCS]; static int nfnames;   /* imports first, then defined funcs */
static int funcidx(const char *bare) {
	int i;
	for (i = 0; i < nfnames; i++) if (!strcmp(fnames[i], bare)) return i;
	die("unknown function");
	return 0;
}
#define MAXGLOBALS 64
static char *gnames[MAXGLOBALS]; static int gtype[MAXGLOBALS]; static long gval[MAXGLOBALS]; static int nglob;
static int globalidx(const char *bare) {
	int i;
	for (i = 0; i < nglob; i++) if (!strcmp(gnames[i], bare)) return i;
	die("unknown global");
	return 0;
}

/* imports */
#define MAXIMPORTS 4096
static char *impfield[MAXIMPORTS]; static int impfieldn[MAXIMPORTS]; static int imptype[MAXIMPORTS]; static int nimp;
/* defined functions: token range covering decls+body */
typedef struct { int tyidx, fstart, fend; } Fn;
static Fn *defs; static int ndefs, capdefs;
/* exports */
#define MAXEXPORTS 16384
static char *expname[MAXEXPORTS]; static int expnamen[MAXEXPORTS]; static int expkind[MAXEXPORTS]; static char *expfref[MAXEXPORTS]; static int expidx[MAXEXPORTS]; static int nexp;
/* element segment */
static char **elemfn; static int nelem, capelem; static long elemoff;
/* data segments */
typedef struct { long addr; unsigned char *bytes; int n; } Data;
static Data *datas; static int ndata, capdata;
static int memmin, tablemin;

/* per-function locals (rebuilt during encode) */
#define MAXLOCALMAP 4096
static char *lmname[MAXLOCALMAP]; static int lmidx[MAXLOCALMAP]; static int nlm;
static unsigned char loctyv[MAXLOCALMAP]; static int nloc;
static int lookuplocal(const char *tok) {
	int i;
	if (tok[0] != '$') return (int)parseu(tok);
	for (i = 0; i < nlm; i++) if (!strcmp(lmname[i], tok + 1)) return lmidx[i];
	die("unknown local");
	return 0;
}

/* parse (param..)/(result..)/(local..) decls starting at ti; fills sig; when
   build!=0 also records local tysig and the name->index map. leaves ti at the
   first body token (or the closing RP for imports). */
static void parse_decls(Sig *sig, int build) {
	int slot = 0;
	sig->np = 0; sig->nr = 0; sig->rt = 0;
	if (build) { nlm = 0; nloc = 0; }
	while (ti < ntoks && toks[ti].t == T_LP) {
		int save = ti;
		char *kw, *name, *ty;
		takelp();
		if (toks[ti].t != T_WORD) { ti = save; break; }
		kw = takeword();
		if (!strcmp(kw, "param")) {
			name = 0;
			if (toks[ti].t == T_WORD && toks[ti].s[0] == '$') name = takeword();
			ty = takeword(); takerp();
			if (sig->np >= MAXPARAMS) die("too many params");
			sig->pt[sig->np++] = (unsigned char)vt(ty);
			if (build && name && nlm < MAXLOCALMAP) { lmname[nlm] = name + 1; lmidx[nlm] = slot; nlm++; }
			slot++;
		} else if (!strcmp(kw, "result")) {
			ty = takeword(); takerp();
			sig->rt = (unsigned char)vt(ty); sig->nr = 1;
		} else if (!strcmp(kw, "local")) {
			name = 0;
			if (toks[ti].t == T_WORD && toks[ti].s[0] == '$') name = takeword();
			ty = takeword(); takerp();
			if (build) {
				if (nloc < MAXLOCALMAP) loctyv[nloc++] = (unsigned char)vt(ty);
				if (name && nlm < MAXLOCALMAP) { lmname[nlm] = name + 1; lmidx[nlm] = slot; nlm++; }
			}
			slot++;
		} else { ti = save; break; }
	}
}

/* ---- instruction opcode tables ---- */
typedef struct { const char *name; unsigned char op; } Op;
static Op simpleops[] = {
	{"unreachable",0x00},{"nop",0x01},{"return",0x0f},{"drop",0x1a},{"select",0x1b},
	{"i32.eqz",0x45},{"i32.eq",0x46},{"i32.ne",0x47},{"i32.lt_s",0x48},{"i32.lt_u",0x49},
	{"i32.gt_s",0x4a},{"i32.gt_u",0x4b},{"i32.le_s",0x4c},{"i32.le_u",0x4d},{"i32.ge_s",0x4e},{"i32.ge_u",0x4f},
	{"i64.eqz",0x50},{"i64.eq",0x51},{"i64.ne",0x52},{"i64.lt_s",0x53},{"i64.lt_u",0x54},
	{"i64.gt_s",0x55},{"i64.gt_u",0x56},{"i64.le_s",0x57},{"i64.le_u",0x58},{"i64.ge_s",0x59},{"i64.ge_u",0x5a},
	{"f32.eq",0x5b},{"f32.ne",0x5c},{"f32.lt",0x5d},{"f32.gt",0x5e},{"f32.le",0x5f},{"f32.ge",0x60},
	{"f64.eq",0x61},{"f64.ne",0x62},{"f64.lt",0x63},{"f64.gt",0x64},{"f64.le",0x65},{"f64.ge",0x66},
	{"i32.clz",0x67},{"i32.ctz",0x68},{"i32.popcnt",0x69},{"i32.add",0x6a},{"i32.sub",0x6b},{"i32.mul",0x6c},
	{"i32.div_s",0x6d},{"i32.div_u",0x6e},{"i32.rem_s",0x6f},{"i32.rem_u",0x70},{"i32.and",0x71},{"i32.or",0x72},
	{"i32.xor",0x73},{"i32.shl",0x74},{"i32.shr_s",0x75},{"i32.shr_u",0x76},{"i32.rotl",0x77},{"i32.rotr",0x78},
	{"i64.clz",0x79},{"i64.ctz",0x7a},{"i64.popcnt",0x7b},{"i64.add",0x7c},{"i64.sub",0x7d},{"i64.mul",0x7e},
	{"i64.div_s",0x7f},{"i64.div_u",0x80},{"i64.rem_s",0x81},{"i64.rem_u",0x82},{"i64.and",0x83},{"i64.or",0x84},
	{"i64.xor",0x85},{"i64.shl",0x86},{"i64.shr_s",0x87},{"i64.shr_u",0x88},{"i64.rotl",0x89},{"i64.rotr",0x8a},
	{"f32.abs",0x8b},{"f32.neg",0x8c},{"f32.ceil",0x8d},{"f32.floor",0x8e},{"f32.trunc",0x8f},{"f32.nearest",0x90},
	{"f32.sqrt",0x91},{"f32.add",0x92},{"f32.sub",0x93},{"f32.mul",0x94},{"f32.div",0x95},{"f32.min",0x96},
	{"f32.max",0x97},{"f32.copysign",0x98},
	{"f64.abs",0x99},{"f64.neg",0x9a},{"f64.ceil",0x9b},{"f64.floor",0x9c},{"f64.trunc",0x9d},{"f64.nearest",0x9e},
	{"f64.sqrt",0x9f},{"f64.add",0xa0},{"f64.sub",0xa1},{"f64.mul",0xa2},{"f64.div",0xa3},{"f64.min",0xa4},
	{"f64.max",0xa5},{"f64.copysign",0xa6},
	{"i32.wrap_i64",0xa7},{"i32.trunc_f32_s",0xa8},{"i32.trunc_f32_u",0xa9},{"i32.trunc_f64_s",0xaa},{"i32.trunc_f64_u",0xab},
	{"i64.extend_i32_s",0xac},{"i64.extend_i32_u",0xad},{"i64.trunc_f32_s",0xae},{"i64.trunc_f32_u",0xaf},
	{"i64.trunc_f64_s",0xb0},{"i64.trunc_f64_u",0xb1},
	{"f32.convert_i32_s",0xb2},{"f32.convert_i32_u",0xb3},{"f32.convert_i64_s",0xb4},{"f32.convert_i64_u",0xb5},
	{"f32.demote_f64",0xb6},
	{"f64.convert_i32_s",0xb7},{"f64.convert_i32_u",0xb8},{"f64.convert_i64_s",0xb9},{"f64.convert_i64_u",0xba},
	{"f64.promote_f32",0xbb},
	{"i32.reinterpret_f32",0xbc},{"i64.reinterpret_f64",0xbd},{"f32.reinterpret_i32",0xbe},{"f64.reinterpret_i64",0xbf},
	{"i32.extend8_s",0xc0},{"i32.extend16_s",0xc1},{"i64.extend8_s",0xc2},{"i64.extend16_s",0xc3},{"i64.extend32_s",0xc4},
	{0,0}
};
typedef struct { const char *name; unsigned char op; int align; } MemOp;
static MemOp memops[] = {
	{"i32.load",0x28,2},{"i64.load",0x29,3},{"f32.load",0x2a,2},{"f64.load",0x2b,3},
	{"i32.load8_s",0x2c,0},{"i32.load8_u",0x2d,0},{"i32.load16_s",0x2e,1},{"i32.load16_u",0x2f,1},
	{"i64.load8_s",0x30,0},{"i64.load8_u",0x31,0},{"i64.load16_s",0x32,1},{"i64.load16_u",0x33,1},
	{"i64.load32_s",0x34,2},{"i64.load32_u",0x35,2},
	{"i32.store",0x36,2},{"i64.store",0x37,3},{"f32.store",0x38,2},{"f64.store",0x39,3},
	{"i32.store8",0x3a,0},{"i32.store16",0x3b,1},{"i64.store8",0x3c,0},{"i64.store16",0x3d,1},{"i64.store32",0x3e,2},
	{0,0,0}
};

/* ---- control-flow label scope stack (for br/br_if/br_table brdepth) ---- */
#define MAXSCOPE 4096
static char *scope[MAXSCOPE]; static int nscope;
static int brdepth(const char *name) {
	int j;
	for (j = nscope - 1; j >= 0; j--) if (!strcmp(scope[j], name)) return (nscope - 1) - j;
	die("unresolved branch label");
	return 0;
}

/* encode the instructions of one function from ti up to (but not including) end */
static void encode_body(Bytes *fn, int end) {
	static char empty[] = "";
	nscope = 0;
	while (ti < end) {
		char *w;
		Op *o; MemOp *m;
		if (toks[ti].t != T_WORD) die("expected instruction");
		w = takeword();
		if (!strcmp(w, "block")) { wb(fn, 0x02); wb(fn, 0x40); scope[nscope++] = takeword() + 1; }
		else if (!strcmp(w, "loop")) { wb(fn, 0x03); wb(fn, 0x40); scope[nscope++] = takeword() + 1; }
		else if (!strcmp(w, "if")) { wb(fn, 0x04); wb(fn, 0x40); scope[nscope++] = empty; }
		else if (!strcmp(w, "else")) { wb(fn, 0x05); }
		else if (!strcmp(w, "end")) { wb(fn, 0x0b); if (nscope > 0) nscope--; }
		else if (!strcmp(w, "br")) { wb(fn, 0x0c); uleb(fn, (unsigned long)brdepth(takeword() + 1)); }
		else if (!strcmp(w, "br_if")) { wb(fn, 0x0d); uleb(fn, (unsigned long)brdepth(takeword() + 1)); }
		else if (!strcmp(w, "br_table")) {
			char *lbl[MAXSCOPE]; int k = 0, i;
			while (ti < end && toks[ti].t == T_WORD && toks[ti].s[0] == '$') lbl[k++] = takeword() + 1;
			if (k < 1) die("empty br_table");
			wb(fn, 0x0e);
			uleb(fn, (unsigned long)(k - 1));
			for (i = 0; i < k - 1; i++) uleb(fn, (unsigned long)brdepth(lbl[i]));
			uleb(fn, (unsigned long)brdepth(lbl[k - 1]));
		}
		else if (!strcmp(w, "call")) { wb(fn, 0x10); uleb(fn, (unsigned long)funcidx(takeword() + 1)); }
		else if (!strcmp(w, "call_indirect")) {
			Sig s; char *kw, *ty;
			s.np = 0; s.nr = 0; s.rt = 0;
			while (ti < end && toks[ti].t == T_LP) {
				takelp(); kw = takeword();
				if (!strcmp(kw, "param")) { ty = takeword(); takerp(); if (s.np < MAXPARAMS) s.pt[s.np++] = (unsigned char)vt(ty); }
				else if (!strcmp(kw, "result")) { ty = takeword(); takerp(); s.rt = (unsigned char)vt(ty); s.nr = 1; }
				else die("bad call_indirect typeuse");
			}
			wb(fn, 0x11); uleb(fn, (unsigned long)interntype(&s)); wb(fn, 0x00);
		}
		else if (!strcmp(w, "local.get") || !strcmp(w, "local.set") || !strcmp(w, "local.tee")) {
			int op = w[6] == 'g' ? 0x20 : w[6] == 's' ? 0x21 : 0x22;
			wb(fn, op); uleb(fn, (unsigned long)lookuplocal(takeword()));
		}
		else if (!strcmp(w, "global.get") || !strcmp(w, "global.set")) {
			int op = w[7] == 'g' ? 0x23 : 0x24;
			wb(fn, op); uleb(fn, (unsigned long)globalidx(takeword() + 1));
		}
		else if (!strcmp(w, "i32.const")) { wb(fn, 0x41); sleb(fn, i32val(takeword())); }
		else if (!strcmp(w, "i64.const")) { wb(fn, 0x42); sleb(fn, i64val(takeword())); }
		else if (!strcmp(w, "f32.const")) { wb(fn, 0x43); f32bytes(fn, takeword()); }
		else if (!strcmp(w, "f64.const")) { wb(fn, 0x44); f64bytes(fn, takeword()); }
		else if (!strcmp(w, "memory.copy")) { wb(fn, 0xfc); uleb(fn, 10); wb(fn, 0x00); wb(fn, 0x00); }
		else if (!strcmp(w, "memory.fill")) { wb(fn, 0xfc); uleb(fn, 11); wb(fn, 0x00); }
		else {
			for (m = memops; m->name; m++) if (!strcmp(w, m->name)) break;
			if (m->name) {
				int align = m->align; unsigned long offset = 0;
				while (ti < end && toks[ti].t == T_WORD &&
				       (strncmp(toks[ti].s, "offset=", 7) == 0 || strncmp(toks[ti].s, "align=", 6) == 0)) {
					char *a = takeword();
					if (a[0] == 'o') offset = parseu(a + 7);
					else align = (int)parseu(a + 6);
				}
				wb(fn, m->op); uleb(fn, (unsigned long)align); uleb(fn, offset);
			} else {
				for (o = simpleops; o->name; o++) if (!strcmp(w, o->name)) break;
				if (!o->name) { fprint(stderr, "rcc: wasm-bin: unknown instruction `%s'\n", w); exit(EXIT_FAILURE); }
				wb(fn, o->op);
			}
		}
	}
}

/* ---- data string unescape ---- */
static int hexv(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return 0;
}
static void decode_data(const char *s, int n, unsigned char **out, int *outn) {
	Bytes b; int i = 0;
	memset(&b, 0, sizeof b);
	while (i < n) {
		if (s[i] == '\\' && i + 2 < n + 1 && i + 1 < n) {
			wb(&b, (hexv((unsigned char)s[i+1]) << 4) | hexv((unsigned char)s[i+2]));
			i += 3;
		} else { wb(&b, (unsigned char)s[i]); i++; }
	}
	*out = b.p; *outn = b.len;
}

static void addfn(char *bare) { if (nfnames >= MAXFUNCS) die("too many functions"); fnames[nfnames++] = bare; }

/* ---- pass 1: parse the module structure ---- */
static void parse_module(void) {
	takelp();
	if (!isword("module")) die("expected (module");
	takeword();
	while (ti < ntoks && toks[ti].t != T_RP) {
		char *kw;
		takelp();
		kw = takeword();
		if (!strcmp(kw, "import")) {
			int fn; char *field; char *name; Sig s;
			(void)takestr(&fn);             /* "env" */
			field = takestr(&fn);           /* field name */
			takelp(); if (!isword("func")) die("import: expected func"); takeword();
			name = takeword();              /* $name */
			parse_decls(&s, 0);
			takerp();                       /* close func */
			takerp();                       /* close import */
			if (nimp >= MAXIMPORTS) die("too many imports");
			impfield[nimp] = field; impfieldn[nimp] = fn; imptype[nimp] = interntype(&s); nimp++;
			addfn(name + 1);
		} else if (!strcmp(kw, "memory")) { memmin = (int)parseu(takeword()); takerp(); }
		else if (!strcmp(kw, "table")) { tablemin = (int)parseu(takeword()); (void)takeword(); takerp(); }
		else if (!strcmp(kw, "global")) {
			char *name = takeword();
			takelp(); (void)takeword(); /* mut */ if (nglob < MAXGLOBALS) gtype[nglob] = vt(takeword()); else die("too many globals"); takerp();
			takelp(); (void)takeword(); /* i32.const */ gval[nglob] = i32val(takeword()); takerp();
			takerp();
			gnames[nglob] = name + 1; nglob++;
		} else if (!strcmp(kw, "func")) {
			char *name = takeword(); Sig s; int brdepthp;
			int fstart = ti;
			parse_decls(&s, 0);
			if (capdefs <= ndefs) { capdefs = capdefs ? capdefs * 2 : 1024; defs = realloc(defs, capdefs * sizeof *defs); if (!defs) die("oom"); }
			defs[ndefs].tyidx = interntype(&s);
			defs[ndefs].fstart = fstart;
			brdepthp = 0;
			while (ti < ntoks) {
				if (toks[ti].t == T_LP) brdepthp++;
				else if (toks[ti].t == T_RP) { if (brdepthp == 0) break; brdepthp--; }
				ti++;
			}
			defs[ndefs].fend = ti;
			takerp();
			ndefs++;
			addfn(name + 1);
		} else if (!strcmp(kw, "export")) {
			int sn; char *nm = takestr(&sn);
			takelp();
			if (nexp >= MAXEXPORTS) die("too many exports");
			expname[nexp] = nm; expnamen[nexp] = sn;
			if (isword("func")) { takeword(); expkind[nexp] = 0; expfref[nexp] = takeword() + 1; }
			else if (isword("table")) { takeword(); expkind[nexp] = 1; expidx[nexp] = (int)parseu(takeword()); }
			else if (isword("memory")) { takeword(); expkind[nexp] = 2; expidx[nexp] = (int)parseu(takeword()); }
			else die("bad export");
			takerp(); takerp();
			nexp++;
		} else if (!strcmp(kw, "elem")) {
			takelp(); (void)takeword(); /* i32.const */ elemoff = i32val(takeword()); takerp();
			while (ti < ntoks && toks[ti].t == T_WORD) {
				if (capelem <= nelem) { capelem = capelem ? capelem * 2 : 256; elemfn = realloc(elemfn, capelem * sizeof *elemfn); if (!elemfn) die("oom"); }
				elemfn[nelem++] = takeword() + 1;
			}
			takerp();
		} else if (!strcmp(kw, "data")) {
			long addr; int sn; char *raw; unsigned char *bytes; int bn;
			takelp(); (void)takeword(); /* i32.const */ addr = i32val(takeword()); takerp();
			raw = takestr(&sn);
			decode_data(raw, sn, &bytes, &bn);
			takerp();
			if (capdata <= ndata) { capdata = capdata ? capdata * 2 : 1024; datas = realloc(datas, capdata * sizeof *datas); if (!datas) die("oom"); }
			datas[ndata].addr = addr; datas[ndata].bytes = bytes; datas[ndata].n = bn; ndata++;
		} else die("unknown module field");
	}
	takerp();
}

/* frame a section into out, only if it has content */
static void section(Bytes *out, int id, Bytes *payload) {
	if (payload->len == 0) return;
	wb(out, id);
	uleb(out, (unsigned long)payload->len);
	wbn(out, payload->p, payload->len);
}

unsigned char *wasm_assemble(const char *text, int *outlen) {
	Bytes out, tsec, isec, fsec, tabsec, msec, gsec, esec, elsec, csec, dsec;
	int i, j;

	toks = 0; ntoks = captoks = ti = 0;
	ntysig = nfnames = nimp = ndefs = nglob = nexp = nelem = ndata = 0;
	defs = 0; capdefs = 0; elemfn = 0; capelem = 0; datas = 0; capdata = 0;
	memmin = 256; tablemin = 0; elemoff = 0;

	tokenize(text);
	parse_module();

	memset(&csec, 0, sizeof csec);
	/* code section first: encoding bodies interns call_indirect tysig */
	uleb(&csec, (unsigned long)ndefs);
	for (i = 0; i < ndefs; i++) {
		Bytes fn; Sig s; int g;
		memset(&fn, 0, sizeof fn);
		ti = defs[i].fstart;
		parse_decls(&s, 1);            /* rebuild locals + name map */
		/* locals vec: group consecutive equal tysig */
		{
			int ngroups = 0, k;
			for (k = 0; k < nloc; ) { int t = loctyv[k]; k++; while (k < nloc && loctyv[k] == t) k++; ngroups++; }
			uleb(&fn, (unsigned long)ngroups);
			for (k = 0; k < nloc; ) { int t = loctyv[k], c = 0; while (k < nloc && loctyv[k] == t) { k++; c++; } uleb(&fn, (unsigned long)c); wb(&fn, t); }
		}
		encode_body(&fn, defs[i].fend);
		wb(&fn, 0x0b);                 /* function end */
		g = 0; (void)g;
		uleb(&csec, (unsigned long)fn.len);
		wbn(&csec, fn.p, fn.len);
		free(fn.p);
	}

	/* type section */
	memset(&tsec, 0, sizeof tsec);
	uleb(&tsec, (unsigned long)ntysig);
	for (i = 0; i < ntysig; i++) {
		wb(&tsec, 0x60);
		uleb(&tsec, (unsigned long)tysig[i].np);
		for (j = 0; j < tysig[i].np; j++) wb(&tsec, tysig[i].pt[j]);
		uleb(&tsec, (unsigned long)tysig[i].nr);
		if (tysig[i].nr) wb(&tsec, tysig[i].rt);
	}
	/* import section */
	memset(&isec, 0, sizeof isec);
	uleb(&isec, (unsigned long)nimp);
	for (i = 0; i < nimp; i++) {
		wname(&isec, "env", 3);
		wname(&isec, impfield[i], impfieldn[i]);
		wb(&isec, 0x00);
		uleb(&isec, (unsigned long)imptype[i]);
	}
	/* function section */
	memset(&fsec, 0, sizeof fsec);
	uleb(&fsec, (unsigned long)ndefs);
	for (i = 0; i < ndefs; i++) uleb(&fsec, (unsigned long)defs[i].tyidx);
	/* table section */
	memset(&tabsec, 0, sizeof tabsec);
	uleb(&tabsec, 1); wb(&tabsec, 0x70); wb(&tabsec, 0x00); uleb(&tabsec, (unsigned long)tablemin);
	/* memory section */
	memset(&msec, 0, sizeof msec);
	uleb(&msec, 1); wb(&msec, 0x00); uleb(&msec, (unsigned long)memmin);
	/* global section */
	memset(&gsec, 0, sizeof gsec);
	uleb(&gsec, (unsigned long)nglob);
	for (i = 0; i < nglob; i++) { wb(&gsec, gtype[i]); wb(&gsec, 0x01); wb(&gsec, 0x41); sleb(&gsec, gval[i]); wb(&gsec, 0x0b); }
	/* export section */
	memset(&esec, 0, sizeof esec);
	uleb(&esec, (unsigned long)nexp);
	for (i = 0; i < nexp; i++) {
		wname(&esec, expname[i], expnamen[i]);
		wb(&esec, (unsigned char)expkind[i]);
		uleb(&esec, (unsigned long)(expkind[i] == 0 ? funcidx(expfref[i]) : expidx[i]));
	}
	/* element section */
	memset(&elsec, 0, sizeof elsec);
	if (nelem > 0) {
		uleb(&elsec, 1);
		wb(&elsec, 0x00); wb(&elsec, 0x41); sleb(&elsec, elemoff); wb(&elsec, 0x0b);
		uleb(&elsec, (unsigned long)nelem);
		for (i = 0; i < nelem; i++) uleb(&elsec, (unsigned long)funcidx(elemfn[i]));
	}
	/* data section */
	memset(&dsec, 0, sizeof dsec);
	uleb(&dsec, (unsigned long)ndata);
	for (i = 0; i < ndata; i++) {
		wb(&dsec, 0x00); wb(&dsec, 0x41); sleb(&dsec, datas[i].addr); wb(&dsec, 0x0b);
		uleb(&dsec, (unsigned long)datas[i].n);
		wbn(&dsec, datas[i].bytes, datas[i].n);
	}

	/* assemble the module in section-id order */
	memset(&out, 0, sizeof out);
	wb(&out, 0x00); wb(&out, 0x61); wb(&out, 0x73); wb(&out, 0x6d);
	wb(&out, 0x01); wb(&out, 0x00); wb(&out, 0x00); wb(&out, 0x00);
	section(&out, 1, &tsec);
	section(&out, 2, &isec);
	section(&out, 3, &fsec);
	section(&out, 4, &tabsec);
	section(&out, 5, &msec);
	section(&out, 6, &gsec);
	section(&out, 7, &esec);
	section(&out, 9, &elsec);
	section(&out, 10, &csec);
	if (ndata > 0) section(&out, 11, &dsec);

	*outlen = out.len;
	return out.p;
}
