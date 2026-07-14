#ifndef SMALLOS_EDITOR_MODEL_H
#define SMALLOS_EDITOR_MODEL_H

#include <stdint.h>

#define EDITOR_LINE_MAX 256u
#define EDITOR_PATH_MAX 256u

typedef struct {
    char** lines;
    uint32_t count;
    uint32_t cap;
    int dirty;
    char path[EDITOR_PATH_MAX];
} editor_model_t;

void editor_model_init(editor_model_t* model, const char* path);
void editor_model_destroy(editor_model_t* model);
int editor_model_load(editor_model_t* model);
int editor_model_save(editor_model_t* model);
uint32_t editor_model_line_length(const editor_model_t* model, uint32_t row);
int editor_model_ensure_line(editor_model_t* model, uint32_t row);
int editor_model_insert_line(editor_model_t* model, uint32_t row,
                             const char* text);
int editor_model_delete_lines(editor_model_t* model, uint32_t first,
                              uint32_t last);
void editor_model_clear(editor_model_t* model);
int editor_model_insert_char(editor_model_t* model, uint32_t row,
                             uint32_t column, char ch);
int editor_model_split_line(editor_model_t* model, uint32_t row,
                            uint32_t column);
int editor_model_join_next(editor_model_t* model, uint32_t row);
int editor_model_backspace(editor_model_t* model, uint32_t* row,
                           uint32_t* column);
int editor_model_delete(editor_model_t* model, uint32_t row,
                        uint32_t column);
int editor_model_delete_range(editor_model_t* model,
                              uint32_t first_row, uint32_t first_column,
                              uint32_t last_row, uint32_t last_column);

#endif
