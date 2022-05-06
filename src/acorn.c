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

/*** data ***/
struct EditorRow {
    int size;
    int render_size;
    char* chars;
    char* render;
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
    struct termios default_termios;
};

struct EditorConfig e;

/*** prototypes ***/
void editor_set_status_message(const char* fmt, ...);
void editor_refresh_screen();
char* editor_prompt(char* prompt);

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

    if (c == '\x1b') {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch(seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }

        return '\x1b';
    } else {
        return c;
    }

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
}

void editor_insert_row(int at, char* s, size_t len) {
    if (at < 0 || at > e.num_rows) return;

    e.row = realloc(e.row, sizeof(struct EditorRow) * (e.num_rows + 1));
    memmove(&e.row[at + 1], &e.row[at], sizeof(struct EditorRow) * (e.num_rows - at));

    e.row[at].size = len;
    e.row[at].chars = malloc(len + 1);
    memcpy(e.row[at].chars, s, len);
    e.row[at].chars[len] = '\0';
    
    e.row[at].render_size = 0;
    e.row[at].render = NULL;
    editor_update_row(&e.row[at]);

    e.num_rows++;
    e.dirty++;
}

void editor_free_row(struct EditorRow* row) {
    free(row->render);
    free(row->chars);
}

void editor_del_row(int at) {
    if (at < 0 || at >= e.num_rows) return;
    editor_free_row(&e.row[at]);
    memmove(&e.row[at], &e.row[at + 1], sizeof(struct EditorRow) * (e.num_rows - at - 1));
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
    if (e.cursor_y == e.num_rows) {
        editor_insert_row(e.num_rows, "", 0);
    }
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
        e.filename = editor_prompt("Save as: %s (ESC to cancel)");
        if (e.filename == NULL) {
            editor_set_status_message("Save aborted");
            return;
        }
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
            append_buffer_append(ab, &e.row[file_row].render[e.col_offset], len);
        }

        append_buffer_append(ab, "\x1b[K", 3); //clear to end of line
        append_buffer_append(ab, "\r\n", 2);
    }
}

void editor_draw_status_bar(struct AppendBuffer* ab) {
    append_buffer_append(ab, "\x1b[7m", 4); // switch to inverted color

    char status[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
            e.filename ? e.filename : "[No Name]", e.num_rows, e.dirty ? "(modified)": "");
    char rstatus[80];
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", e.cursor_y + 1, e.num_rows);

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
    append_buffer_append(ab, "\x1b[m", 3); //switch back to normal color mode
    append_buffer_append(ab, "\r\n", 2);
}

void editor_draw_message_bar(struct AppendBuffer* ab) {
    append_buffer_append(ab, "\x1b[K", 3); //seems like using <esc>[K then resets cursor back to beginning of line
    int msglen = strlen(e.status_msg);
    append_buffer_append(ab, "\x1b[38;2;210;242;227m", 19); //change text color
    if (msglen > e.screencols) msglen = e.screencols;
    if (msglen && time(NULL) - e.status_msg_time < 5)
        append_buffer_append(ab, e.status_msg, msglen);
}

void editor_refresh_screen() {
    editor_scroll();

    struct AppendBuffer ab = APPEND_BUFFER_INIT;

    append_buffer_append(&ab, "\x1b[?25l", 6); //hide cursor before redrawing to avoid flicker
    append_buffer_append(&ab, "\x1b[H", 3);  //move cursor back to beginning

    append_buffer_append(&ab, "\x1b[48;2;18;27;40m", 16); //draw background color (38;2 is text, 48;2 is background
    append_buffer_append(&ab, "\x1b[38;2;210;242;227m", 19); //draw text color (38;2 is text, 48;2 is background

    editor_draw_rows(&ab);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    //draw cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (e.cursor_y - e.row_offset) + 1, 
                                              (e.render_x - e.col_offset) + 1); //adding 1 since terminal uses starting index 1
    append_buffer_append(&ab, buf, strlen(buf));

    append_buffer_append(&ab, "\x1b[?25h", 6); //show cursor
         
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
char* editor_prompt(char* prompt) {
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
            free(buffer);
            return NULL;
        } else if (c == '\r') {
            if (buffer_len != 0) {
                editor_set_status_message("");
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
    }
}

void editor_move_cursor(int key) {
    struct EditorRow* row = (e.cursor_y >= e.num_rows) ? NULL : &e.row[e.cursor_y];

    switch(key) {
        case ARROW_LEFT:
            if (e.cursor_x != 0) {
                e.cursor_x--;
            } else if (e.cursor_y > 0) {
                e.cursor_y--;
                e.cursor_x = e.row[e.cursor_y].size;
            }
            break;
        case ARROW_DOWN:
            if (e.cursor_y < e.num_rows) e.cursor_y++;
            break;
        case ARROW_UP:
            if (e.cursor_y != 0) e.cursor_y--;
            break;
        case ARROW_RIGHT:
            if (row && e.cursor_x < row->size) {
                e.cursor_x++;
            } else if (row && e.cursor_x == row->size) {
                e.cursor_y++;
                e.cursor_x = 0;
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

    switch(c) {
        case '\r':
            editor_insert_new_line();
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
            break;
        default:
            editor_insert_char(c);
            break;
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

    editor_set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
