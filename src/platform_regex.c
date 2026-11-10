/*
 * platform_regex.c - fnmatch() and POSIX extended regular expressions for
 * systems whose C library has neither (Windows).
 *
 * The search tool needs to answer one question per line: does this pattern
 * match anywhere in it? So the regex engine reports match / no match only.
 * It compiles the pattern to a small program and runs every alternative in
 * lockstep (a Thompson/Pike simulation), which takes time proportional to
 * pattern size x line length whatever the pattern - there is no
 * backtracking for a model-written pattern to blow up.
 *
 * Supported: | ( ) * + ? {m} {m,} {m,n} . ^ $ [...] with ranges and
 * [:classes:], and the GNU escapes \w \W \s \S \b \B \< \>. As in the C
 * locale, matching is byte by byte and case folding is ASCII only.
 * Back-references are not supported.
 *
 * On systems with <regex.h> this file compiles to nothing.
 */

#ifdef _WIN32

#include "platform.h"

#include <ctype.h>

/* Taking the address of isalpha() and friends, which live in the runtime DLL. */
#pragma warning(disable : 4232)

/* ------------------------------------------------------------------------
 * fnmatch
 * --------------------------------------------------------------------- */

/* Match one "[...]" at p against c. Returns the position after the closing
 * bracket, sets *matched; NULL if the bracket never closes. */
static const char *match_bracket(const char *p, unsigned char c, bool *matched)
{
	bool negate = false, found = false;
	p++; /* '[' */
	if (*p == '!' || *p == '^') {
		negate = true;
		p++;
	}
	for (bool first = true; *p && (first || *p != ']'); first = false) {
		unsigned char lo = (unsigned char)*p++;
		if (lo == '\\' && *p)
			lo = (unsigned char)*p++;
		unsigned char hi = lo;
		if (p[0] == '-' && p[1] && p[1] != ']') {
			hi = (unsigned char)p[1];
			p += 2;
			if (hi == '\\' && *p)
				hi = (unsigned char)*p++;
		}
		if (c >= lo && c <= hi)
			found = true;
	}
	if (*p != ']')
		return NULL;
	*matched = found != negate;
	return p + 1;
}

/* flags are ignored: '*' and '?' match '/' and a leading '.', as with 0. */
int fnmatch(const char *pattern, const char *string, int flags)
{
	(void)flags;
	const char *p = pattern, *s = string;
	const char *star_p = NULL, *star_s = NULL; /* where to resume after '*' */

	for (;;) {
		bool ok = false;
		if (*p == '*') {
			star_p = ++p;
			star_s = s;
			continue;
		}
		if (*s == '\0') {
			if (*p == '\0')
				return 0;
		} else if (*p == '?') {
			p++;
			ok = true;
		} else if (*p == '[') {
			bool matched = false;
			const char *end = match_bracket(p, (unsigned char)*s, &matched);
			if (end) {
				p = end;
				ok = matched;
			} else if (*s == '[') { /* unclosed: an ordinary '[' */
				p++;
				ok = true;
			}
		} else if (*p != '\0') {
			const char *lit = (*p == '\\' && p[1]) ? p + 1 : p;
			if (*lit == *s) {
				p = lit + 1;
				ok = true;
			}
		}

		if (ok) {
			s++;
			continue;
		}
		/* Mismatch: let the last '*' swallow one more character. */
		if (!star_p || *star_s == '\0')
			return FNM_NOMATCH;
		p = star_p;
		s = ++star_s;
	}
}

/* ------------------------------------------------------------------------
 * Regex: syntax tree
 * --------------------------------------------------------------------- */

enum {
	REG_BADPAT = 2,
	REG_ECTYPE = 4,
	REG_EESCAPE = 5,
	REG_ESUBREG = 6,
	REG_EBRACK = 7,
	REG_EPAREN = 8,
	REG_BADBR = 10,
	REG_ERANGE = 11,
	REG_ESPACE = 12,
	REG_BADRPT = 13,
};

#define REGEX_DUP_MAX	255   /* largest count in {m,n} */
#define REGEX_MAX_INSTS 50000 /* compiled size limit */
#define REGEX_MAX_DEPTH 200   /* nesting of groups */
#define REGEX_MAX_CHARS 4096  /* pattern length */

enum node_kind {
	N_EMPTY,
	N_CHAR,	  /* value = byte */
	N_ANY,
	N_SET,	  /* value = index into sets */
	N_ASSERT, /* value = enum op of the assertion */
	N_CAT,
	N_ALT,
	N_REPEAT, /* left, min..max (max < 0: unbounded) */
};

struct node {
	enum node_kind kind;
	int value;
	int min, max;
	struct node *left, *right;
};

struct charset {
	unsigned char bits[32];
};

enum op {
	OP_CHAR,
	OP_ANY,
	OP_SET,
	OP_SPLIT, /* continue at both x and y */
	OP_JUMP,
	OP_BOL,
	OP_EOL,
	OP_WORD_BOUNDARY,
	OP_NOT_WORD_BOUNDARY,
	OP_WORD_START,
	OP_WORD_END,
	OP_MATCH,
};

struct inst {
	unsigned char op;
	int x, y;
};

struct program {
	struct inst *insts;
	int count, cap;
	struct charset *sets;
	int set_count, set_cap;
	bool ignore_case;
};

struct parser {
	const char *p;
	struct program *prog;
	int depth;
	int error;
};

static void set_add(struct charset *set, unsigned char c)
{
	set->bits[c >> 3] |= (unsigned char)(1u << (c & 7));
}

static bool set_has(const struct charset *set, unsigned char c)
{
	return (set->bits[c >> 3] >> (c & 7)) & 1;
}

static void free_node(struct node *n)
{
	if (!n)
		return;
	free_node(n->left);
	free_node(n->right);
	free(n);
}

static struct node *new_node(struct parser *ps, enum node_kind kind, int value,
			     struct node *left, struct node *right)
{
	struct node *n = ps->error ? NULL : calloc(1, sizeof(*n));
	if (!n) {
		if (!ps->error)
			ps->error = REG_ESPACE;
		free_node(left);
		free_node(right);
		return NULL;
	}
	n->kind = kind;
	n->value = value;
	n->left = left;
	n->right = right;
	return n;
}

static int new_set(struct parser *ps)
{
	struct program *prog = ps->prog;
	if (prog->set_count == prog->set_cap) {
		int cap = prog->set_cap ? prog->set_cap * 2 : 8;
		struct charset *tmp = realloc(prog->sets, (size_t)cap * sizeof(*tmp));
		if (!tmp) {
			ps->error = REG_ESPACE;
			return -1;
		}
		prog->sets = tmp;
		prog->set_cap = cap;
	}
	memset(&prog->sets[prog->set_count], 0, sizeof(struct charset));
	return prog->set_count++;
}

/* ------------------------------------------------------------------------
 * Regex: parser
 * --------------------------------------------------------------------- */

static bool is_word(unsigned char c)
{
	return isalnum(c) || c == '_';
}

static bool class_has(const char *name, size_t len, unsigned char c)
{
	static const struct {
		const char *name;
		int (*test)(int);
	} classes[] = {
		{"alpha", isalpha}, {"digit", isdigit}, {"alnum", isalnum},
		{"upper", isupper}, {"lower", islower}, {"space", isspace},
		{"blank", isblank}, {"punct", ispunct}, {"print", isprint},
		{"graph", isgraph}, {"cntrl", iscntrl}, {"xdigit", isxdigit},
	};
	for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); i++)
		if (strlen(classes[i].name) == len && strncmp(classes[i].name, name, len) == 0)
			return c < 128 && classes[i].test(c);
	return false;
}

static bool class_known(const char *name, size_t len)
{
	/* Every class contains at least one of these. */
	return class_has(name, len, 'a') || class_has(name, len, 'A') ||
	       class_has(name, len, '0') || class_has(name, len, ' ') ||
	       class_has(name, len, '!') || class_has(name, len, '\t') ||
	       class_has(name, len, 1);
}

/* Add both cases of every letter in the set. */
static void set_fold_case(struct charset *set)
{
	for (int c = 'a'; c <= 'z'; c++) {
		if (set_has(set, (unsigned char)c) || set_has(set, (unsigned char)toupper(c))) {
			set_add(set, (unsigned char)c);
			set_add(set, (unsigned char)toupper(c));
		}
	}
}

/* A set node for one of \w \W \s \S. */
static struct node *escape_class(struct parser *ps, char which)
{
	int index = new_set(ps);
	if (index < 0)
		return NULL;
	struct charset *set = &ps->prog->sets[index];
	for (int c = 0; c < 256; c++) {
		bool in = (which == 'w' || which == 'W') ? is_word((unsigned char)c)
							 : (c < 128 && isspace(c));
		if (in == (which == 'w' || which == 's'))
			set_add(set, (unsigned char)c);
	}
	return new_node(ps, N_SET, index, NULL, NULL);
}

/* "[...]"; ps->p is just past the '['. A backslash is an ordinary character
 * here, as POSIX says. */
static struct node *parse_bracket(struct parser *ps)
{
	int index = new_set(ps);
	if (index < 0)
		return NULL;
	struct charset set;
	memset(&set, 0, sizeof(set));

	bool negate = false;
	if (*ps->p == '^') {
		negate = true;
		ps->p++;
	}

	for (bool first = true; first || *ps->p != ']'; first = false) {
		if (*ps->p == '\0') {
			ps->error = REG_EBRACK;
			return NULL;
		}
		if (ps->p[0] == '[' && ps->p[1] == ':') {
			const char *name = ps->p + 2;
			const char *end = strstr(name, ":]");
			if (!end) {
				ps->error = REG_EBRACK;
				return NULL;
			}
			size_t len = (size_t)(end - name);
			if (!class_known(name, len)) {
				ps->error = REG_ECTYPE;
				return NULL;
			}
			for (int c = 0; c < 256; c++)
				if (class_has(name, len, (unsigned char)c))
					set_add(&set, (unsigned char)c);
			ps->p = end + 2;
			continue;
		}

		unsigned char lo = (unsigned char)*ps->p++;
		unsigned char hi = lo;
		if (ps->p[0] == '-' && ps->p[1] != ']' && ps->p[1] != '\0') {
			hi = (unsigned char)ps->p[1];
			ps->p += 2;
			if (hi < lo) {
				ps->error = REG_ERANGE;
				return NULL;
			}
		}
		for (int c = lo; c <= hi; c++)
			set_add(&set, (unsigned char)c);
	}
	ps->p++; /* ']' */

	if (ps->prog->ignore_case)
		set_fold_case(&set);
	if (negate)
		for (size_t i = 0; i < sizeof(set.bits); i++)
			set.bits[i] = (unsigned char)~set.bits[i];
	ps->prog->sets[index] = set;
	return new_node(ps, N_SET, index, NULL, NULL);
}

static struct node *parse_alternation(struct parser *ps);

static struct node *parse_atom(struct parser *ps)
{
	char c = *ps->p++;
	switch (c) {
	case '(': {
		if (++ps->depth > REGEX_MAX_DEPTH) {
			ps->error = REG_ESPACE;
			return NULL;
		}
		struct node *inner = parse_alternation(ps);
		ps->depth--;
		if (!ps->error && *ps->p != ')')
			ps->error = REG_EPAREN;
		if (ps->error) {
			free_node(inner);
			return NULL;
		}
		ps->p++;
		return inner;
	}
	case '[':
		return parse_bracket(ps);
	case '.':
		return new_node(ps, N_ANY, 0, NULL, NULL);
	case '^':
		return new_node(ps, N_ASSERT, OP_BOL, NULL, NULL);
	case '$':
		return new_node(ps, N_ASSERT, OP_EOL, NULL, NULL);
	case '*':
	case '+':
	case '?':
		ps->error = REG_BADRPT; /* nothing to repeat */
		return NULL;
	case '\\':
		c = *ps->p++;
		switch (c) {
		case '\0':
			ps->p--;
			ps->error = REG_EESCAPE;
			return NULL;
		case 'w':
		case 'W':
		case 's':
		case 'S':
			return escape_class(ps, c);
		case 'b':
			return new_node(ps, N_ASSERT, OP_WORD_BOUNDARY, NULL, NULL);
		case 'B':
			return new_node(ps, N_ASSERT, OP_NOT_WORD_BOUNDARY, NULL, NULL);
		case '<':
			return new_node(ps, N_ASSERT, OP_WORD_START, NULL, NULL);
		case '>':
			return new_node(ps, N_ASSERT, OP_WORD_END, NULL, NULL);
		case '`':
			return new_node(ps, N_ASSERT, OP_BOL, NULL, NULL);
		case '\'':
			return new_node(ps, N_ASSERT, OP_EOL, NULL, NULL);
		default:
			if (c >= '1' && c <= '9') {
				ps->error = REG_ESUBREG;
				return NULL;
			}
			return new_node(ps, N_CHAR, (unsigned char)c, NULL, NULL);
		}
	default:
		return new_node(ps, N_CHAR, (unsigned char)c, NULL, NULL);
	}
}

/* Parse "{m}", "{m,}" or "{m,n}" at ps->p (which is at the '{'). Returns
 * false, consuming nothing, if this is not a valid bound: the brace is then
 * an ordinary character, which is what someone searching code for "{" means. */
static bool parse_bound(struct parser *ps, int *min, int *max)
{
	const char *q = ps->p + 1;
	if (!isdigit((unsigned char)*q))
		return false;
	long lo = 0, hi;
	while (isdigit((unsigned char)*q) && lo <= REGEX_DUP_MAX)
		lo = lo * 10 + (*q++ - '0');
	hi = lo;
	if (*q == ',') {
		q++;
		if (*q == '}') {
			hi = -1;
		} else {
			if (!isdigit((unsigned char)*q))
				return false;
			hi = 0;
			while (isdigit((unsigned char)*q) && hi <= REGEX_DUP_MAX)
				hi = hi * 10 + (*q++ - '0');
		}
	}
	if (*q != '}')
		return false;
	if (lo > REGEX_DUP_MAX || hi > REGEX_DUP_MAX || (hi >= 0 && hi < lo)) {
		ps->error = REG_BADBR;
		return false;
	}
	ps->p = q + 1;
	*min = (int)lo;
	*max = (int)hi;
	return true;
}

static struct node *parse_piece(struct parser *ps)
{
	struct node *atom = parse_atom(ps);
	while (atom && !ps->error) {
		int min, max;
		if (*ps->p == '*') {
			min = 0, max = -1;
			ps->p++;
		} else if (*ps->p == '+') {
			min = 1, max = -1;
			ps->p++;
		} else if (*ps->p == '?') {
			min = 0, max = 1;
			ps->p++;
		} else if (*ps->p != '{' || !parse_bound(ps, &min, &max)) {
			break;
		}
		atom = new_node(ps, N_REPEAT, 0, atom, NULL);
		if (atom) {
			atom->min = min;
			atom->max = max;
		}
	}
	if (ps->error) {
		free_node(atom);
		return NULL;
	}
	return atom;
}

static struct node *parse_branch(struct parser *ps)
{
	struct node *branch = NULL;
	/* A ')' with no '(' open is an ordinary character. */
	while (*ps->p && *ps->p != '|' && (*ps->p != ')' || ps->depth == 0)) {
		struct node *piece = parse_piece(ps);
		if (!piece) {
			free_node(branch);
			return NULL;
		}
		branch = branch ? new_node(ps, N_CAT, 0, branch, piece) : piece;
		if (!branch)
			return NULL;
	}
	return branch ? branch : new_node(ps, N_EMPTY, 0, NULL, NULL);
}

static struct node *parse_alternation(struct parser *ps)
{
	struct node *left = parse_branch(ps);
	while (left && *ps->p == '|') {
		ps->p++;
		struct node *right = parse_branch(ps);
		if (!right) {
			free_node(left);
			return NULL;
		}
		left = new_node(ps, N_ALT, 0, left, right);
	}
	return left;
}

/* ------------------------------------------------------------------------
 * Regex: code generation
 * --------------------------------------------------------------------- */

/* Append an instruction; returns its index, or -1 (ps->error set). */
static int emit(struct parser *ps, enum op op, int x, int y)
{
	struct program *prog = ps->prog;
	if (ps->error)
		return -1;
	if (prog->count >= REGEX_MAX_INSTS) {
		ps->error = REG_ESPACE;
		return -1;
	}
	if (prog->count == prog->cap) {
		int cap = prog->cap ? prog->cap * 2 : 64;
		struct inst *tmp = realloc(prog->insts, (size_t)cap * sizeof(*tmp));
		if (!tmp) {
			ps->error = REG_ESPACE;
			return -1;
		}
		prog->insts = tmp;
		prog->cap = cap;
	}
	prog->insts[prog->count].op = (unsigned char)op;
	prog->insts[prog->count].x = x;
	prog->insts[prog->count].y = y;
	return prog->count++;
}

static void generate(struct parser *ps, const struct node *n)
{
	struct program *prog = ps->prog;
	if (ps->error)
		return;

	switch (n->kind) {
	case N_EMPTY:
		break;
	case N_CHAR:
		emit(ps, OP_CHAR, prog->ignore_case ? tolower(n->value) : n->value, 0);
		break;
	case N_ANY:
		emit(ps, OP_ANY, 0, 0);
		break;
	case N_SET:
		emit(ps, OP_SET, n->value, 0);
		break;
	case N_ASSERT:
		emit(ps, (enum op)n->value, 0, 0);
		break;
	case N_CAT:
		generate(ps, n->left);
		generate(ps, n->right);
		break;
	case N_ALT: {
		int split = emit(ps, OP_SPLIT, 0, 0);
		generate(ps, n->left);
		int jump = emit(ps, OP_JUMP, 0, 0);
		if (ps->error)
			return;
		prog->insts[split].x = split + 1;
		prog->insts[split].y = prog->count;
		generate(ps, n->right);
		if (!ps->error)
			prog->insts[jump].x = prog->count;
		break;
	}
	case N_REPEAT:
		/* x{2,4} becomes  x x (x (x)?)?  and x{2,} becomes  x x x*  */
		for (int i = 0; i < n->min; i++)
			generate(ps, n->left);
		if (n->max < 0) {
			int split = emit(ps, OP_SPLIT, 0, 0);
			generate(ps, n->left);
			emit(ps, OP_JUMP, split, 0);
			if (ps->error)
				return;
			prog->insts[split].x = split + 1;
			prog->insts[split].y = prog->count;
		} else {
			int first = prog->count;
			for (int i = n->min; i < n->max; i++) {
				int split = emit(ps, OP_SPLIT, 0, -1);
				generate(ps, n->left);
				if (ps->error)
					return;
				prog->insts[split].x = split + 1;
			}
			/* Every "give up here" exit leads to the same place. */
			for (int i = first; i < prog->count; i++)
				if (prog->insts[i].op == OP_SPLIT && prog->insts[i].y == -1)
					prog->insts[i].y = prog->count;
		}
		break;
	}
}

/* ------------------------------------------------------------------------
 * Regex: matcher
 * --------------------------------------------------------------------- */

struct thread_list {
	int *pcs;
	int count;
};

struct matcher {
	const struct program *prog;
	const unsigned char *text;
	size_t len;
	int *mark;  /* per instruction: the step it was last added in */
	int *stack; /* explicit stack for following jumps and splits */
};

static bool assertion_holds(const struct matcher *m, enum op op, size_t at)
{
	bool before = at > 0 && is_word(m->text[at - 1]);
	bool after = at < m->len && is_word(m->text[at]);
	switch (op) {
	case OP_BOL:
		return at == 0;
	case OP_EOL:
		return at == m->len;
	case OP_WORD_BOUNDARY:
		return before != after;
	case OP_NOT_WORD_BOUNDARY:
		return before == after;
	case OP_WORD_START:
		return !before && after;
	case OP_WORD_END:
		return before && !after;
	default:
		return false;
	}
}

/*
 * Add pc to list, following everything that consumes no input. step is a
 * number unique to this position in the text; an instruction already seen
 * in this step is not added again, which is also what makes loops over an
 * empty match terminate. Returns true if OP_MATCH is reachable.
 */
static bool add_thread(struct matcher *m, struct thread_list *list, int pc, size_t at,
		       int step)
{
	int depth = 0;
	m->stack[depth++] = pc;
	while (depth > 0) {
		pc = m->stack[--depth];
		if (m->mark[pc] == step)
			continue;
		m->mark[pc] = step;

		const struct inst *in = &m->prog->insts[pc];
		switch (in->op) {
		case OP_MATCH:
			return true;
		case OP_JUMP:
			m->stack[depth++] = in->x;
			break;
		case OP_SPLIT:
			m->stack[depth++] = in->y;
			m->stack[depth++] = in->x;
			break;
		case OP_CHAR:
		case OP_ANY:
		case OP_SET:
			list->pcs[list->count++] = pc;
			break;
		default:
			if (assertion_holds(m, (enum op)in->op, at))
				m->stack[depth++] = pc + 1;
			break;
		}
	}
	return false;
}

static bool run(const struct program *prog, const char *text)
{
	struct matcher m;
	m.prog = prog;
	m.text = (const unsigned char *)text;
	m.len = strlen(text);

	/* One allocation: marks (n), two thread lists (n each), and the stack.
	 * An instruction is expanded once per step and pushes at most two
	 * others, so the stack never holds more than 2n + 1 entries. */
	size_t n = (size_t)prog->count;
	int *memory = malloc((n * 5 + 1) * sizeof(int));
	if (!memory)
		return false;
	m.mark = memory;
	struct thread_list current = {memory + n, 0};
	struct thread_list next = {memory + 2 * n, 0};
	m.stack = memory + 3 * n;
	for (size_t i = 0; i < n; i++)
		m.mark[i] = -1;

	bool matched = false;
	for (size_t at = 0;; at++) {
		/* A match may start at every position. */
		if (add_thread(&m, &current, 0, at, (int)at)) {
			matched = true;
			break;
		}
		if (at == m.len)
			break;

		unsigned char c = m.text[at];
		unsigned char folded = prog->ignore_case ? (unsigned char)tolower(c) : c;
		next.count = 0;
		for (int i = 0; i < current.count && !matched; i++) {
			int pc = current.pcs[i];
			const struct inst *in = &prog->insts[pc];
			bool ok = in->op == OP_ANY ||
				  (in->op == OP_CHAR && in->x == folded) ||
				  (in->op == OP_SET && set_has(&prog->sets[in->x], c));
			/* Threads for position at+1 are marked with step at+1; the
			 * start thread added at the top of the loop shares it. */
			if (ok && add_thread(&m, &next, pc + 1, at + 1, (int)at + 1))
				matched = true;
		}
		if (matched)
			break;
		struct thread_list swap = current;
		current = next;
		next = swap;
	}

	free(memory);
	return matched;
}

/* ------------------------------------------------------------------------
 * Regex: public interface
 * --------------------------------------------------------------------- */

static void free_program(struct program *prog)
{
	if (!prog)
		return;
	free(prog->insts);
	free(prog->sets);
	free(prog);
}

int regcomp(regex_t *preg, const char *pattern, int cflags)
{
	preg->program = NULL;
	if (!(cflags & REG_EXTENDED))
		return REG_BADPAT; /* basic regular expressions are not implemented */
	if (strlen(pattern) > REGEX_MAX_CHARS)
		return REG_ESPACE;

	struct program *prog = calloc(1, sizeof(*prog));
	if (!prog)
		return REG_ESPACE;
	prog->ignore_case = (cflags & REG_ICASE) != 0;

	struct parser ps = {pattern, prog, 0, 0};
	struct node *tree = parse_alternation(&ps);
	if (tree) {
		generate(&ps, tree);
		emit(&ps, OP_MATCH, 0, 0);
	}
	free_node(tree);

	if (ps.error) {
		free_program(prog);
		return ps.error;
	}
	preg->program = prog;
	return 0;
}

int regexec(const regex_t *preg, const char *string, size_t nmatch, regmatch_t pmatch[],
	    int eflags)
{
	(void)nmatch;
	(void)pmatch;
	(void)eflags;
	return run(preg->program, string) ? 0 : REG_NOMATCH;
}

size_t regerror(int errcode, const regex_t *preg, char *errbuf, size_t errbuf_size)
{
	(void)preg;
	const char *msg;
	switch (errcode) {
	case 0:
		msg = "Success";
		break;
	case REG_NOMATCH:
		msg = "No match";
		break;
	case REG_ECTYPE:
		msg = "Invalid character class name";
		break;
	case REG_EESCAPE:
		msg = "Trailing backslash";
		break;
	case REG_ESUBREG:
		msg = "Back-references are not supported";
		break;
	case REG_EBRACK:
		msg = "Unmatched [, [^, [:, [., or [=";
		break;
	case REG_EPAREN:
		msg = "Unmatched ( or \\(";
		break;
	case REG_BADBR:
		msg = "Invalid content of \\{\\}";
		break;
	case REG_ERANGE:
		msg = "Invalid range end";
		break;
	case REG_ESPACE:
		msg = "Regular expression too big";
		break;
	case REG_BADRPT:
		msg = "Invalid preceding regular expression";
		break;
	default:
		msg = "Invalid regular expression";
		break;
	}
	if (errbuf && errbuf_size > 0)
		snprintf(errbuf, errbuf_size, "%s", msg);
	return strlen(msg) + 1;
}

void regfree(regex_t *preg)
{
	if (preg) {
		free_program(preg->program);
		preg->program = NULL;
	}
}

#endif /* _WIN32 */
