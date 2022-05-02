/*** includes ***/

//NOTE: these three lines make sure 'getline()' works with all compilers
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
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
#define CTRL_KEY(k) ((k) & 0x1f)

enum EditorKey {
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
    struct EditorRow* row;
    char* filename;
    char status_msg[80];
    time_t status_msg_time;
    struct termios default_termios;
};

struct EditorConfig e;

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

void editor_append_row(char* s, size_t len) {
    e.row = realloc(e.row, sizeof(struct EditorRow) * (e.num_rows + 1));

    int at = e.num_rows;
    e.row[at].size = len;
    e.row[at].chars = malloc(len + 1);
    memcpy(e.row[at].chars, s, len);
    e.row[at].chars[len] = '\0';
    
    e.row[at].render_size = 0;
    e.row[at].render = NULL;
    editor_update_row(&e.row[at]);

    e.num_rows++;
}

/*** file i/o ***/
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

        editor_append_row(line, linelen);
    }
    free(line);
    fclose(fp);
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
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
            e.filename ? e.filename : "[No Name]", e.num_rows);
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
    append_buffer_append(ab, "\x1b[K", 3);
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
    int c = editor_read_key();

    switch(c) {
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME_KEY:
            e.cursor_x = 0;
            break;
        case END_KEY:
            if (e.cursor_y < e.num_rows)
                e.cursor_x = e.row[e.cursor_y].size;
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
    }
}

/*** init ***/
void init_editor() {
    e.cursor_x = 0;
    e.cursor_y = 0;
    e.render_x = 0;
    e.row_offset = 0;
    e.col_offset = 0;
    e.num_rows = 0;
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

    editor_set_status_message("HELP: Ctrl-Q = quit");

    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
