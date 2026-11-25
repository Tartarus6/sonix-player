#ifndef CALC_ENGINE_H
#define CALC_ENGINE_H

// ---------------------------------------------------------------------------
// calc_engine — the arithmetic behind the calculator page
//
// Behaves like the classic iPod touch / iPhone OS calculator in portrait:
// immediate execution (2 + 3 × 4 = gives 20, each operator applies what is
// pending first), up to 9 digits typed, a second "=" repeats the last
// operation, "+ =" uses the shown number as both operands, and a memory
// register driven by mc / m+ / m- / mr.
//
// Pure logic: no LVGL, no config. The page reads calc_display() after every
// key and shows it.
// ---------------------------------------------------------------------------

#include <stdbool.h>

typedef enum {
	CALC_OP_NONE = 0,
	CALC_OP_ADD,
	CALC_OP_SUB,
	CALC_OP_MUL,
	CALC_OP_DIV,
} calc_op_t;

// Clears everything, memory included (the page's start state).
void calc_reset(void);

void calc_digit(int digit); // 0-9
void calc_dot(void);
void calc_op(calc_op_t op);
void calc_equals(void);
// The "C" key: clears the number, the pending operation and any error. Keeps
// the memory, as the original did.
void calc_clear(void);
void calc_negate(void); // the "±" key

void calc_mem_clear(void);  // mc
void calc_mem_add(void);    // m+
void calc_mem_sub(void);    // m-
void calc_mem_recall(void); // mr

// What the display shows: the number being typed exactly as typed, otherwise
// the last result with at most 9 significant digits. NULL-terminated, never
// NULL. When calc_error() is true the page shows its own translated text.
const char *calc_display(void);
bool calc_error(void);
// The operator waiting for its second operand, for the page to highlight its
// key the way the original did. CALC_OP_NONE once a digit is typed.
calc_op_t calc_highlighted_op(void);
// True while the memory holds a value, for the "M" mark on the display.
bool calc_has_memory(void);

#endif /* CALC_ENGINE_H */
