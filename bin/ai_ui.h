/* The chat's screen and prompt; no network or model state lives here. */
#pragma once

#include "pt/sys.h"

#define AI_INPUT_MAX 512

struct ai_ui;

struct ai_ui *ai_ui_open(int cols, int rows);
void ai_ui_close(struct ai_ui *ui);
void ai_ui_mode(struct ai_ui *ui, const char *mode);
void ai_ui_context(struct ai_ui *ui, const char *folder, const char *model, const char *effort);
void ai_ui_usage(struct ai_ui *ui, double cost, uint64_t tokens, bool partial);
void ai_ui_reset(struct ai_ui *ui);
int ai_ui_choose(struct ai_ui *ui, const char *title, const char *const *items, int n, int selected);
void ai_ui_write(struct ai_ui *ui, const char *s, size_t n);
void ai_ui_draw(struct ai_ui *ui, bool force);
/* Busy status keeps the draft editable; readline alone submits it. */
void ai_ui_status(struct ai_ui *ui, const char *status);
/* Scroll keys are handled here, other keys go to the dialog's caller. */
int ai_ui_key(struct ai_ui *ui, int timeout_ms);
bool ai_ui_poll(struct ai_ui *ui);
/* Edit a draft without submitting it, including during a tool dialog. */
void ai_ui_edit_key(struct ai_ui *ui, int key);
/* -2 for Esc (the caller stops jobs first), -1 for end of input. */
int ai_ui_readline(struct ai_ui *ui, char *out, size_t size);
void ai_ui_draft(struct ai_ui *ui, const char *text);
