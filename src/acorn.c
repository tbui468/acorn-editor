/*** includes ***/

//NOTE: these three lines make sure 'getline()' works with all compilers
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/
#define ACORN_VERSION "0.0.1"
#define ACORN_TAB_STOP 4
#define ACORN_QUIT_TIMES 3
#define CTRL_KEY(k) ((k) & 0x1f)

#define COLOR_BACKGROUND "\x1b[48;2;0;0;0m\0"
#define COLOR_FOREGROUND "\x1b[38;2;134;214;247m\0"
#define COLOR_RED "\x1b[38;2;220;87;107m\0"
#define COLOR_YELLOW "\x1b[38;2;214;181;101m\0" 
#define COLOR_ORANGE "\x1b[38;2;240;141;74m\0"
#define COLOR_GREY "\x1b[38;2;65;101;105m\0"
#define COLOR_GREEN "\x1b[38;2;27;183;171m\0"
#define COLOR_BLUE "\x1b[38;2;134;214;247m\0"

#define HIDE_CURSOR "\x1b[?25l\0"
#define SHOW_CURSOR "\x1b[?25h\0"

enum EditorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};


enum EditorMode {
    MODE_COMMAND,
    MODE_INSERT 
};

enum EditorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** data ***/
struct EditorSyntax {
    char* file_type; //this will display in editor status bar
    char** file_match; //an array of char* extensions for this filetype
    char** keywords;
    char* singleline_comment_start;
    char* multiline_comment_start;
    char* multiline_comment_end;
    int flags;
};

struct EditorRow {
    int idx;
    int size;
    int render_size;
    char* chars;
    char* render;
    unsigned char* hl;
    int hl_open_comment;
};

struct EditorConfig {
    //NOTE: cursor_x and cursor_y now refers to position in file, NOT position on screen
    int cursor_x, cursor_y;
    int render_x;
    int row_offset;
    int col_offset;
    int screenrows;
    int screencols;
    int num_rows;
    int dirty;
    struct EditorRow* row;
    char* filename;
    char status_msg[80];
    time_t status_msg_time;
    struct EditorSyntax* syntax;
    struct termios default_termios;
    unsigned char mode;
};

struct EditorConfig e;

/*** filetypes ***/
char* C_HL_extensions[] =  { ".c", ".h", ".c", NULL };
char* C_HL_keywords[] = {
    "auto", "break", "case", "const", "continue", "default", "do", "else",
    "enum", "extern", "for", "goto", "if", "register", "return", "sizeof", 
    "static", "struct", "switch", "typedef", "union", "volatile", "while",

    "char|", "double|", "float|", "int|", "long|", "short|", "signed|", "unsigned|", "void|",
    NULL
};

char* PY_HL_extensions[] =  { ".py", NULL };
char* PY_HL_keywords[] = {
    "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else", "except", "finally", "for",
    "from", "global", "if", "import", "in", "is", "lambda", "nonlocal", "not",
    "or", "pass", "raise", "return", "try", "while", "with", "yield",

    "None|", "True|", "False|"
};

struct EditorSyntax HLDB[] = {
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "python",
        PY_HL_extensions,
        PY_HL_keywords,
        "#", "", "",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/
void editor_set_status_message(const char* fmt, ...);
void editor_refresh_screen();
char* editor_prompt(char* prompt, void (*callback)(char*, int));

/*** terminal ***/
void die(const char* s) {
    write(STDOUT_FILENO, "\x1b[2J", 4); //clear screen
    write(STDOUT_FILENO, "\x1b[H", 3); //reposition cursor to top-left of screen

    perror(s);
    exit(1);
}

void disable_raw_mode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &e.default_termios) == -1) {
        die("tcsetattr");
    }
}

void enable_raw_mode() {
    if (tcgetattr(STDIN_FILENO, &e.default_termios) == -1) {
        die("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = e.default_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        die("tcsetattr");
    }
}

int editor_read_key() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    return c;
}

//currently used to determine window size
int get_cursor_position(int* rows, int* cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int get_window_size(int* rows, int* cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlighting ***/
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];:", c) != NULL;
}

void editor_update_syntax(struct EditorRow* row) {
    row->hl = realloc(row->hl, row->render_size);
    memset(row->hl, HL_NORMAL, row->render_size);

    if (e.syntax == NULL) return;

    char** keywords = e.syntax->keywords;

    //check if comment characters were set in EditorSyntax
    char* scs = e.syntax->singleline_comment_start;
    char* mcs = e.syntax->multiline_comment_start;
    char* mce = e.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && e.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->render_size) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->hl[i], HL_COMMENT, row->render_size - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (e.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->render_size) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue; 
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (e.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                   (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0; //0 means we are currently highlighting a number
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                        is_separator(row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }

                //checking if keyword loop was exited using 'break'
                if (keywords[j] != NULL) {
                    prev_sep = 0;
                    continue;
                }
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < e.num_rows)
        editor_update_syntax(&e.row[row->idx + 1]);
}

int editor_syntax_to_color(int hl) {
    switch (hl) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 36;
        case HL_KEYWORD1: return 33;
        case HL_KEYWORD2: return 32;
        case HL_STRING: return 35;
        case HL_NUMBER: return 31;
        case HL_MATCH: return 34;
        default: return 37; //HL_NORMAL is handled separately, so this doesn't do anything...?
    }
}

char* editor_color_to_string(int color) {
    switch (color) {
        case 31: return COLOR_ORANGE;
        case 33: return COLOR_YELLOW;
        case 32: return COLOR_GREEN;
        case 36: return COLOR_GREY;
        case 35: return COLOR_RED;
        default: return COLOR_FOREGROUND;
    }
}

void editor_select_syntax_highlight() {
    e.syntax = NULL;
    if (e.filename == NULL) return;

    char* ext = strrchr(e.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct EditorSyntax* s = &HLDB[j];
        unsigned int i = 0;
        while (s->file_match[i]) {
            int is_ext = (s->file_match[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->file_match[i])) || 
                    (!is_ext && strstr(e.filename, s->file_match[i]))) {
                e.syntax = s;

                //highlight current file for when user saves as an extension
                int filerow;
                for (filerow = 0; filerow < e.num_rows; filerow++) {
                    editor_update_syntax(&e.row[filerow]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/
int editor_row_cursor_x_to_render_x(struct EditorRow* row, int cursor_x) {
    int render_x = 0;
    int j;
    for (j = 0; j < cursor_x; j++) {
        if (row->chars[j] == '\t')
            render_x += (ACORN_TAB_STOP - 1) - (render_x % ACORN_TAB_STOP);
        render_x++;
    }

    return render_x;
}

int editor_row_render_x_to_cursor_x(struct EditorRow* row, int render_x) {
    int current_render_x = 0;
    int cursor_x;
    for (cursor_x = 0; cursor_x < row->size; cursor_x++) {
        if (row->chars[cursor_x] == '\t')
            current_render_x += (ACORN_TAB_STOP - 1) - (current_render_x % ACORN_TAB_STOP);
        current_render_x++;

        if (current_render_x > render_x) return cursor_x;
    }

    return cursor_x;
}

void editor_update_row(struct EditorRow* row) {
    //count tabs
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (ACORN_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % ACORN_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->render_size = idx;

    editor_update_syntax(row);
}

void editor_insert_row(int at, char* s, size_t len) {
    if (at < 0 || at > e.num_rows) return;

    e.row = realloc(e.row, sizeof(struct EditorRow) * (e.num_rows + 1));
    memmove(&e.row[at + 1], &e.row[at], sizeof(struct EditorRow) * (e.num_rows - at));
    for (int j = at + 1; j <= e.num_rows; j++) e.row[j].idx++;

    e.row[at].idx = at;

    e.row[at].size = len;
    e.row[at].chars = malloc(len + 1);
    memcpy(e.row[at].chars, s, len);
    e.row[at].chars[len] = '\0';
    
    e.row[at].render_size = 0;
    e.row[at].render = NULL;
    e.row[at].hl = NULL;
    e.row[at].hl_open_comment = 0;
    editor_update_row(&e.row[at]);

    e.num_rows++;
    e.dirty++;
}

void editor_free_row(struct EditorRow* row) {
    free(row->render);
    free(row->chars);
    free(row->hl);
}

void editor_del_row(int at) {
    if (at < 0 || at >= e.num_rows) return;
    editor_free_row(&e.row[at]);
    memmove(&e.row[at], &e.row[at + 1], sizeof(struct EditorRow) * (e.num_rows - at - 1));
    for (int j = at; j < e.num_rows - 1; j++) e.row[j].idx--;
    e.num_rows--;
    e.dirty++;
}

void editor_row_insert_char(struct EditorRow* row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2); //one for inserted character and one for null terminator
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editor_update_row(row);
    e.dirty++;
}

void editor_row_append_string(struct EditorRow* row, char* s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    editor_update_row(row);
    e.dirty++;
}

void editor_row_del_char(struct EditorRow* row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editor_update_row(row);
    e.dirty++;
}

/*** editor operations ***/
void editor_insert_char(int c) {
    editor_row_insert_char(&e.row[e.cursor_y], e.cursor_x, c);
    e.cursor_x++;
}

void editor_insert_new_line() {
    if (e.cursor_x == 0) {
        editor_insert_row(e.cursor_y, "", 0);
    } else {
        struct EditorRow* row = &e.row[e.cursor_y];
        editor_insert_row(e.cursor_y + 1, &row->chars[e.cursor_x], row->size - e.cursor_x);
        row = &e.row[e.cursor_y]; //need to reassigned row in case realloc in 'editor_insert_row' moved some memory
        row->size = e.cursor_x;
        row->chars[row->size] = '\0';
        editor_update_row(row);
    }
    e.cursor_y++;
    e.cursor_x = 0;
}

void editor_del_char() {
    if (e.cursor_y == e.num_rows) return;
    if (e.cursor_x == 0 && e.cursor_y == 0) return;

    struct EditorRow* row = &e.row[e.cursor_y];
    if (e.cursor_x > 0) {
        editor_row_del_char(row, e.cursor_x - 1);
        e.cursor_x--;
    } else {
        e.cursor_x = e.row[e.cursor_y - 1].size;
        editor_row_append_string(&e.row[e.cursor_y - 1], row->chars, row->size);
        editor_del_row(e.cursor_y);
        e.cursor_y--;
    }
}

/*** file i/o ***/
char* editor_rows_to_string(int* buffer_length) {
    int total_length = 0;
    int j;
    for (j = 0; j < e.num_rows; j++)
        total_length += e.row[j].size + 1;
    *buffer_length = total_length;

    char* buffer = malloc(total_length);
    char* p = buffer;
    for (j = 0; j < e.num_rows; j++) {
        memcpy(p, e.row[j].chars, e.row[j].size);
        p += e.row[j].size;
        *p = '\n';
        p++;
    }
    return buffer;
}

void editor_open(char* filename) {
    free(e.filename);
    e.filename = strdup(filename);

    editor_select_syntax_highlight();

    FILE* fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char* line = NULL;
    size_t line_capacity = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &line_capacity, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;

        editor_insert_row(e.num_rows, line, linelen);
    }
    free(line);
    fclose(fp);
    e.dirty = 0;
}

void editor_save() {
    if (e.filename == NULL) {
        e.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (e.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
        editor_select_syntax_highlight();
    }

    int len;
    char* buffer = editor_rows_to_string(&len);

    int fd = open(e.filename, O_RDWR | O_CREAT, 0644); //0644 is standard permissions - owner gets read/write, otherwise only read
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buffer, len) == len) {
                close(fd);
                free(buffer);
                e.dirty = 0;
                editor_set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buffer);
    editor_set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/
void editor_find_callback(char* query, int key) {
    static int last_match = -1;
    static int direction = 1; //1 is forward, -1 is backwards

    static int saved_hl_line;
    static char* saved_hl = NULL;

    if (saved_hl) {
        memcpy(e.row[saved_hl_line].hl, saved_hl, e.row[saved_hl_line].render_size);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < e.num_rows; i++) {
        //loop 'current'
        current += direction;
        if (current == -1) current = e.num_rows - 1;
        else if (current == e.num_rows) current = 0;

        struct EditorRow* row = &e.row[current];
        char* match = strstr(row->render, query);
        if (match) {
            last_match = current;
            e.cursor_y = current;
            e.cursor_x = editor_row_render_x_to_cursor_x(row, match - row->render);
            e.row_offset = e.num_rows; //setting offset so that next screen refresh will scroll up so query is at top of screen

            saved_hl_line = current;
            saved_hl = malloc(row->render_size);
            memcpy(saved_hl, row->hl, row->render_size);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void editor_find() {
    int saved_cx = e.cursor_x;
    int saved_cy = e.cursor_y;
    int saved_coloff = e.col_offset;
    int saved_rowoff = e.row_offset;

    char* query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)", editor_find_callback);

    if (query) {
        free(query);
    } else {
        e.cursor_x = saved_cx;
        e.cursor_y = saved_cy;
        e.col_offset = saved_coloff;
        e.row_offset = saved_rowoff;
    }
}

/*** append buffer ***/

struct AppendBuffer {
    char* buffer;
    int len;
};

#define APPEND_BUFFER_INIT {NULL, 0}

void append_buffer_append(struct AppendBuffer* ab, const char* s, int len) {
    char* new = realloc(ab->buffer, ab->len + len);

    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->buffer = new;
    ab->len += len;
}

void append_buffer_free(struct AppendBuffer* ab) {
    free(ab->buffer);
}

/*** output ***/
void editor_scroll() {
    e.render_x = 0;
    if (e.cursor_y < e.num_rows) {
        e.render_x = editor_row_cursor_x_to_render_x(&e.row[e.cursor_y], e.cursor_x);
    }

    if (e.cursor_y < e.row_offset) {
        e.row_offset = e.cursor_y;
    }
    if (e.cursor_y >= e.row_offset + e.screenrows) {
        e.row_offset = e.cursor_y - e.screenrows + 1;
    }
    if (e.render_x < e.col_offset) {
        e.col_offset = e.render_x;
    }
    if (e.render_x >= e.col_offset + e.screencols) {
        e.col_offset = e.render_x - e.screencols + 1;
    }
}

void invert_colors(struct AppendBuffer* ab) {
    append_buffer_append(ab, "\x1b[7m", 4);
}

void revert_colors(struct AppendBuffer* ab) {
    append_buffer_append(ab, "\x1b[m", 3);
    append_buffer_append(ab, COLOR_FOREGROUND, strlen(COLOR_FOREGROUND));
    append_buffer_append(ab, COLOR_BACKGROUND, strlen(COLOR_BACKGROUND));
}

void editor_draw_rows(struct AppendBuffer* ab) {
    int y;
    for (y = 0; y < e.screenrows; y++) {
        int file_row = y + e.row_offset;
        if (file_row >= e.num_rows) { //draw empty lines
            if (e.num_rows == 0 && y == e.screenrows / 3) {
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                        "80s Sci-Fi Editor -- version %s", ACORN_VERSION);
                if (welcome_len > e.screencols) welcome_len = e.screencols;
                int padding = (e.screencols - welcome_len) / 2;
                if (padding) {
                    append_buffer_append(ab, "~", 1);
                    padding--;
                }
                while (padding--) append_buffer_append(ab, " ", 1);
                append_buffer_append(ab, welcome, welcome_len);
            } else {
                append_buffer_append(ab, "~", 1);
            }
        } else { //draw text in buffer
            int len = e.row[file_row].render_size - e.col_offset;
            if (len < 0) len = 0;
            if (len > e.screencols) len = e.screencols;
            char* c = &e.row[file_row].render[e.col_offset];
            unsigned char* hl = &e.row[file_row].hl[e.col_offset];
            int current_color = -1;
            int j;
            for (j = 0; j < len; j++) {
                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    invert_colors(ab);
                    append_buffer_append(ab, &sym, 1);
                    revert_colors(ab);
                    //reset current color
                    if (current_color != -1) {
                        char* c = editor_color_to_string(current_color);
                        append_buffer_append(ab, c, strlen(c)); 
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        append_buffer_append(ab, COLOR_FOREGROUND, strlen(COLOR_FOREGROUND));
                        current_color = -1;
                    }
                    append_buffer_append(ab, &c[j], 1);
                } else {
                    int color = editor_syntax_to_color(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                      
                        char* c = editor_color_to_string(current_color);
                        append_buffer_append(ab, c, strlen(c)); 
                    }
                    append_buffer_append(ab, &c[j], 1);
                }
            }
            append_buffer_append(ab, COLOR_FOREGROUND, strlen(COLOR_FOREGROUND));
        }

        append_buffer_append(ab, "\x1b[K", 3); //clear to end of line
        append_buffer_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct AppendBuffer* ab) {
    invert_colors(ab);

    char status[80];
    int len = snprintf(status, sizeof(status), "%s %.20s - %d lines %s", e.mode == MODE_INSERT ? "-- INSERT --" : "" ,
            e.filename ? e.filename : "[No Name]", e.num_rows, e.dirty ? "(modified)": "");
    char rstatus[80];
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
            e.syntax ? e.syntax->file_type : "no ft", e.cursor_y + 1, e.num_rows);

    if (len > e.screencols) len = e.screencols;
    append_buffer_append(ab, status, len);
    while (len < e.screencols) {
        if (e.screencols - len == rlen) {
            append_buffer_append(ab, rstatus, rlen);
            break;
        } else {
            append_buffer_append(ab, " ", 1);
            len++;
        }
    }
    revert_colors(ab);
    append_buffer_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct AppendBuffer* ab) {
    append_buffer_append(ab, "\x1b[K", 3); //seems like using <esc>[K then resets cursor back to beginning of line
    int msglen = strlen(e.status_msg);
    if (msglen > e.screencols) msglen = e.screencols;
    if (msglen && time(NULL) - e.status_msg_time < 5)
        append_buffer_append(ab, e.status_msg, msglen);
}

void editor_refresh_screen() {
    editor_scroll();

    struct AppendBuffer ab = APPEND_BUFFER_INIT;

    append_buffer_append(&ab, HIDE_CURSOR, strlen(HIDE_CURSOR)); //to avoid flicker when redrawing
    append_buffer_append(&ab, "\x1b[H", 3);  //move cursor back to beginning

    append_buffer_append(&ab, COLOR_FOREGROUND, strlen(COLOR_FOREGROUND));
    append_buffer_append(&ab, COLOR_BACKGROUND, strlen(COLOR_BACKGROUND));

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    //draw cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (e.cursor_y - e.row_offset) + 1, 
                                              (e.render_x - e.col_offset) + 1); //adding 1 since terminal uses starting index 1
    append_buffer_append(&ab, buf, strlen(buf));

    append_buffer_append(&ab, SHOW_CURSOR, strlen(SHOW_CURSOR));
         
    write(STDOUT_FILENO, ab.buffer, ab.len); 
    append_buffer_free(&ab); 

} 

void editor_set_status_message(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.status_msg, sizeof(e.status_msg), fmt, ap);
    va_end(ap);
    e.status_msg_time = time(NULL);
}

/*** input ***/
char* editor_prompt(char* prompt, void (*callback)(char*, int)) {
    size_t buffer_size = 128;
    char* buffer = malloc(buffer_size);

    size_t buffer_len = 0;
    buffer[0] = '\0';

    while (1) {
        editor_set_status_message(prompt, buffer);
        editor_refresh_screen();

        int c = editor_read_key();

        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buffer_len != 0) buffer[--buffer_len] = '\0';
        } else if (c == '\x1b') {
            editor_set_status_message("");
            if (callback) callback(buffer, c);
            free(buffer);
            return NULL;
        } else if (c == '\r') {
            if (buffer_len != 0) {
                editor_set_status_message("");
                if (callback) callback(buffer, c);
                return buffer;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buffer_len == buffer_size - 1) {
                buffer_size *= 2;
                buffer = realloc(buffer, buffer_size);
            }
            buffer[buffer_len++] = c;
            buffer[buffer_len] = '\0';
        }

        if (callback) callback(buffer, c);
    }
}

void editor_move_cursor(int key) {
    struct EditorRow* row = (e.cursor_y >= e.num_rows) ? NULL : &e.row[e.cursor_y];

    switch(key) {
        case ARROW_LEFT:
            if (e.cursor_x != 0) {
                e.cursor_x--;
            } 
            break;
        case ARROW_DOWN:
            if (e.cursor_y < e.num_rows - 1) e.cursor_y++;
            break;
        case ARROW_UP:
            if (e.cursor_y != 0) e.cursor_y--;
            break;
        case ARROW_RIGHT:
            if (row && e.cursor_x < row->size) {
                e.cursor_x++;
            }
            break;
    }

    row = (e.cursor_y >= e.num_rows) ? NULL : &e.row[e.cursor_y];
    int rowlen = row ? row->size : 0;
    if (e.cursor_x > rowlen) {
        e.cursor_x = rowlen;
    }
}

void editor_process_keypress() {
    static int quit_times = ACORN_QUIT_TIMES;

    int c = editor_read_key();

    if (e.mode == MODE_COMMAND) {
        switch (c) {
            case 'A':
                e.mode = MODE_INSERT;
                if (e.cursor_y < e.num_rows)
                    e.cursor_x = e.row[e.cursor_y].size;
                break;
            case 'G':
                {
                    int times = e.num_rows;
                    while (times--)
                        editor_move_cursor(ARROW_DOWN);
                }
                break;
            case 'a':
                e.mode = MODE_INSERT;
                editor_move_cursor(ARROW_RIGHT);
                break;
            case 'i':
                e.mode = MODE_INSERT;
                break;
            case 'h':
                editor_move_cursor(ARROW_LEFT);
                break;
            case 'j':
                editor_move_cursor(ARROW_DOWN);
                break;
            case 'k':
                editor_move_cursor(ARROW_UP);
                break;
            case 'l':
                editor_move_cursor(ARROW_RIGHT);
                break;
            case 'x':
                editor_move_cursor(ARROW_RIGHT);
                editor_del_char();
                break;
            case ':': {
                char* command = editor_prompt(":%s", NULL);
                if (command == NULL) break;
                int clen = strlen(command);
                if (clen == 1) {
                    switch (command[0]) {
                        case 'w':
                            editor_save();
                            break;
                        case 'q':
                            if (e.dirty) {
                                editor_set_status_message("No write since last change. (Add ! to override).");
                                return;
                            }
                            write(STDOUT_FILENO, "\x1b[2J", 4);
                            write(STDOUT_FILENO, "\x1b[H", 3);
                            exit(0);
                            break;
                        default:
                            break;
                    }
                } else if (clen == 2) {
                    if (command[0] == 'q' && command[1] == '!') {
                        write(STDOUT_FILENO, "\x1b[2J", 4);
                        write(STDOUT_FILENO, "\x1b[H", 3);
                        exit(0);
                        break;
                    }
                }
                //TODO: quit, save, open buffer, swap buffer
                break;
            }
            case '/': {
                char* prompt = editor_prompt("/%s", NULL);
                //TODO: incremental search with 'n' and 'N' to go forward/backward
                break;
            }
            default:
                break;
        }
    } else { //e.mode == MODE_INSERT
        switch(c) {
            case '\r':
                editor_insert_new_line();
                break;
            case CTRL_KEY('c'):
                e.mode = MODE_COMMAND;
                break;
            case CTRL_KEY('q'):
                if (e.dirty && quit_times > 0) {
                    editor_set_status_message("WARNING!!! File has unsaved changes.  Press Ctrl-Q %d more times to quit.", quit_times);
                    quit_times--;
                    return;
                }
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                exit(0);
                break;
            case CTRL_KEY('s'):
                editor_save();
                break;
            case HOME_KEY:
                e.cursor_x = 0;
                break;
            case END_KEY:
                if (e.cursor_y < e.num_rows)
                    e.cursor_x = e.row[e.cursor_y].size;
                break;
            case CTRL_KEY('f'):
                editor_find();
                break;
            case BACKSPACE:
            case CTRL_KEY('h'):
            case DEL_KEY:
                if (c == DEL_KEY) editor_move_cursor(ARROW_RIGHT);
                editor_del_char();
                break;
            case PAGE_UP:
            case PAGE_DOWN: 
                {
                    if (c == PAGE_UP) {
                        e.cursor_y = e.row_offset;
                    } else if (c == PAGE_DOWN) {
                        e.cursor_y = e.row_offset + e.screenrows - 1;
                        if (e.cursor_y > e.num_rows) e.cursor_y = e.num_rows;
                    }

                    int times = e.screenrows;
                    while (times--)
                        editor_move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
                }
                break;
            case ARROW_LEFT:
            case ARROW_DOWN:
            case ARROW_UP:
            case ARROW_RIGHT:
                editor_move_cursor(c);
                break;
            case CTRL_KEY('l'):
            case '\x1b':
                e.mode = MODE_COMMAND;
                break;
            default:
                editor_insert_char(c);
                break;
        }
    }

    quit_times = ACORN_QUIT_TIMES;
}

/*** init ***/
void init_editor() {
    e.cursor_x = 0;
    e.cursor_y = 0;
    e.render_x = 0;
    e.row_offset = 0;
    e.col_offset = 0;
    e.num_rows = 0;
    e.dirty = 0;
    e.row = NULL;
    e.filename = NULL;
    e.status_msg[0] = '\0';
    e.status_msg_time = 0;
    e.syntax = NULL;
    e.mode = MODE_COMMAND;

    if (get_window_size(&e.screenrows, &e.screencols) == -1) {
        die("get_window_size");
    }

    e.screenrows -= 2;
}

int main(int argc, char* argv[]) {
    enable_raw_mode();
    init_editor();
    if (argc >= 2) {
        editor_open(argv[1]);
    }

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
