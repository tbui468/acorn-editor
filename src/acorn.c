/*** includes ***/
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/
#define ACORN_VERSION "0.0.1"
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
struct EditorConfig {
    int cursor_x, cursor_y;
    int screenrows;
    int screencols;
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
void editor_draw_rows(struct AppendBuffer* ab) {
    int y;
    for (y = 0; y < e.screenrows; y++) {
        if (y == e.screenrows / 3) {
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

        append_buffer_append(ab, "\x1b[K", 3); //clear to end of line
        if (y < e.screenrows - 1) {
            append_buffer_append(ab, "\r\n", 2);
        }
    }
}

void editor_refresh_screen() {
    struct AppendBuffer ab = APPEND_BUFFER_INIT;

    append_buffer_append(&ab, "\x1b[?25l", 6); //hide cursor before redrawing to avoid flicker
    append_buffer_append(&ab, "\x1b[H", 3);  //move cursor back to beginning

    append_buffer_append(&ab, "\x1b[48;2;18;27;40m", 16); //draw background color (38;2 is text, 48;2 is background
    append_buffer_append(&ab, "\x1b[38;2;210;242;227m", 19); //draw text color (38;2 is text, 48;2 is background

    editor_draw_rows(&ab);

    //draw cursor
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", e.cursor_y + 1, e.cursor_x + 1); //adding 1 since terminal uses starting index 1
    append_buffer_append(&ab, buf, strlen(buf));

    append_buffer_append(&ab, "\x1b[?25h", 6); //show cursor
         
    write(STDOUT_FILENO, ab.buffer, ab.len); 
    append_buffer_free(&ab); 

} 

/*** input ***/
void editor_move_cursor(int key) {
    switch(key) {
        case ARROW_LEFT:
            if (e.cursor_x != 0) e.cursor_x--;
            break;
        case ARROW_DOWN:
            if (e.cursor_y != e.screenrows - 1) e.cursor_y++;
            break;
        case ARROW_UP:
            if (e.cursor_y != 0) e.cursor_y--;
            break;
        case ARROW_RIGHT:
            if (e.cursor_x != e.screencols - 1) e.cursor_x++;
            break;
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
            e.cursor_x = e.screencols - 1;
            break;
        case PAGE_UP:
        case PAGE_DOWN: 
            {
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

    if (get_window_size(&e.screenrows, &e.screencols) == -1) {
        die("get_window_size");
    }
}

int main() {
    enable_raw_mode();
    init_editor();
    
    while (1) {
        editor_refresh_screen();
        editor_process_keypress();
    }
    return 0;
}
