#include "calc_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIGITS 9      /* digits the user can type, as on the original  */
#define MAX_MAGNITUDE 1e100 /* beyond this a result is shown as an error  */

static char entry[MAX_DIGITS + 4]; /* "-" + digits + "." + NUL             */
static bool typing;     /* entry[] is being edited                          */
static bool operand;    /* the display holds a fresh second operand         */
static double value;    /* the shown number when not typing                 */
static double acc;      /* the left operand of the pending operation        */
static calc_op_t op;    /* the pending operation                            */
static calc_op_t last_op;      /* for a repeated "="                        */
static double last_operand;
static bool err;
static double mem;
static bool mem_set;
static char shown[48];

static double current(void) { return typing ? strtod(entry, NULL) : value; }

static int digit_count(void) {
	int n = 0;
	for (const char *p = entry; *p; p++) {
		if (*p >= '0' && *p <= '9') {
			n++;
		}
	}
	return n;
}

static void set_error(void) {
	err = true;
	typing = false;
	operand = false;
	op = CALC_OP_NONE;
	last_op = CALC_OP_NONE;
	value = 0;
	acc = 0;
}

// Applies `o` and reports overflow and division by zero as an error.
static bool apply(double a, calc_op_t o, double b, double *out) {
	double r;
	switch (o) {
	case CALC_OP_ADD: r = a + b; break;
	case CALC_OP_SUB: r = a - b; break;
	case CALC_OP_MUL: r = a * b; break;
	case CALC_OP_DIV:
		if (b == 0) {
			return false;
		}
		r = a / b;
		break;
	default: r = b; break;
	}
	if (!isfinite(r) || fabs(r) >= MAX_MAGNITUDE) {
		return false;
	}
	// Results that are zero to 9 significant digits next to their operands
	// (0.1 + 0.2 - 0.3) would otherwise show as 5.55111512e-17.
	double scale = fmax(fabs(a), fabs(b));
	if (scale > 0 && fabs(r) < scale * 1e-12) {
		r = 0;
	}
	*out = r;
	return true;
}

static void start_entry(void) {
	strcpy(entry, "0");
	typing = true;
	operand = true;
}

void calc_reset(void) {
	calc_clear();
	mem = 0;
	mem_set = false;
}

void calc_clear(void) {
	strcpy(entry, "0");
	typing = false;
	operand = false;
	value = 0;
	acc = 0;
	op = CALC_OP_NONE;
	last_op = CALC_OP_NONE;
	last_operand = 0;
	err = false;
}

void calc_digit(int digit) {
	if (digit < 0 || digit > 9) {
		return;
	}
	if (err) {
		calc_clear();
	}
	if (!typing) {
		// A repeated "=" still applies to a number typed after a result
		// (2 + 3 = 7 = gives 10), as on the original.
		start_entry();
	}
	if (digit_count() >= MAX_DIGITS) {
		return;
	}
	// Replace a lone leading zero ("0" or "-0") rather than append to it.
	char *zero = entry[0] == '-' ? entry + 1 : entry;
	if (strcmp(zero, "0") == 0) {
		zero[0] = (char)('0' + digit);
		return;
	}
	size_t len = strlen(entry);
	entry[len] = (char)('0' + digit);
	entry[len + 1] = '\0';
}

void calc_dot(void) {
	if (err) {
		calc_clear();
	}
	if (!typing) {
		start_entry();
	}
	if (strchr(entry, '.') || digit_count() >= MAX_DIGITS) {
		return;
	}
	strcat(entry, ".");
}

void calc_op(calc_op_t o) {
	if (err || o == CALC_OP_NONE) {
		return;
	}
	if (op != CALC_OP_NONE && operand) {
		double r;
		if (!apply(acc, op, current(), &r)) {
			set_error();
			return;
		}
		acc = r;
	} else if (op == CALC_OP_NONE) {
		acc = current();
	}
	// With an operator already pending and nothing typed since, a second
	// operator key just replaces it.
	value = acc;
	op = o;
	typing = false;
	operand = false;
	last_op = CALC_OP_NONE;
}

void calc_equals(void) {
	if (err) {
		return;
	}
	double r;
	if (op != CALC_OP_NONE) {
		// "5 + =" adds the shown number to itself, as the original did.
		double rhs = operand ? current() : acc;
		if (!apply(acc, op, rhs, &r)) {
			set_error();
			return;
		}
		last_op = op;
		last_operand = rhs;
		op = CALC_OP_NONE;
	} else if (last_op != CALC_OP_NONE) {
		// "=" again repeats the last operation on the result.
		if (!apply(current(), last_op, last_operand, &r)) {
			set_error();
			return;
		}
	} else {
		r = current();
	}
	value = r;
	acc = r;
	typing = false;
	operand = false;
}

void calc_negate(void) {
	if (err) {
		return;
	}
	if (!typing && op != CALC_OP_NONE) {
		// After an operator, "±" starts the second operand as "-0".
		start_entry();
	}
	if (typing) {
		if (entry[0] == '-') {
			memmove(entry, entry + 1, strlen(entry));
		} else {
			memmove(entry + 1, entry, strlen(entry) + 1);
			entry[0] = '-';
		}
		return;
	}
	value = -value;
	acc = value;
}

void calc_mem_clear(void) {
	mem = 0;
	mem_set = false;
}

static void mem_apply(double sign) {
	if (err) {
		return;
	}
	double v = current();
	double r = mem + sign * v;
	if (!isfinite(r) || fabs(r) >= MAX_MAGNITUDE) {
		set_error();
		return;
	}
	mem = r;
	mem_set = true;
	// The number stays on the display, but the next digit starts a new one.
	value = v;
	typing = false;
}

void calc_mem_add(void) { mem_apply(1); }
void calc_mem_sub(void) { mem_apply(-1); }

void calc_mem_recall(void) {
	if (err) {
		calc_clear();
	}
	value = mem;
	typing = false;
	operand = true; // usable as the second operand of a pending operation
	if (op == CALC_OP_NONE) {
		acc = mem;
	}
}

// At most 9 significant digits, no trailing zeros, and never "-0".
static void format_number(double v, char *out, size_t len) {
	if (v == 0) {
		snprintf(out, len, "0");
		return;
	}
	snprintf(out, len, "%.9g", v);
	// %g picks exponent form past 9 digits. Redo it with 9 significant digits,
	// written the short way the original did: "9.9997e14", not "9.99970000e+14".
	if (strchr(out, 'e')) {
		char buf[24];
		snprintf(buf, sizeof(buf), "%.8e", v);
		char *e = strchr(buf, 'e');
		int exp10 = atoi(e + 1);
		*e = '\0';
		char *end = e - 1;
		while (end > buf && *end == '0') {
			*end-- = '\0';
		}
		if (*end == '.') {
			*end = '\0';
		}
		snprintf(out, len, "%se%d", buf, exp10);
	}
}

const char *calc_display(void) {
	if (err) {
		return "0";
	}
	if (typing) {
		return entry;
	}
	format_number(value, shown, sizeof(shown));
	return shown;
}

bool calc_error(void) { return err; }

calc_op_t calc_highlighted_op(void) { return operand ? CALC_OP_NONE : op; }

bool calc_has_memory(void) { return mem_set; }
