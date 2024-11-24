// hello
// credit to https://viewsourcecode.org/snaptoken/kilo/04.aTextViewer.html
/*** includes ***/
#define _DEFAULT_SOURCE

#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>

/*** prototypes that just gotta be up here to be global ***/
void editorSetStatusMessage(const char *fmt, ...);

/*** defines ***/
#define CTRL_PLUS(k) ((k) & 0x1f)

#define JOUR_VERSION "0.0.1"
#define TAB_STOP 4

#define MSG_SUSTAIN 5
#define SCROLLZONE 10

/*** data ***/
typedef struct erow {
    char *chars;
    int size;
    int rsize;
    char *render;
} erow;

struct editorCfg {
    int mode;
    int lastx, lasty;
    int cx, cy;
    int rx;
    int screenrows;
    int cmdrow;
    int screencols;
    int rowoff;
    int coloff;
    int numrows;
    erow *row;
    char *filename;
    char statusmsg[80];
    time_t msg_time;
    struct termios orig_termios;
};

struct editorCfg cfg;

enum Flag {
    R_NOSCROLL,
    R_SCROLL,
};

enum Mode {
    NORMAL,
    INSERT,
    COMMAND
};

enum Key {
    MOVE_LEFT  = 'h',
    MOVE_DOWN  = 'j',
    MOVE_UP    = 'k',
    MOVE_RIGHT = 'l',
    DELETE     = 127,
    TOP,
    BOTTOM,
    CMDLINE,
    LINE_START,
    LINE_END,
};

/*** append buffer ***/
struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL)
        return;

    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** terminal ***/
void die(const char *str) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(str);
    exit(1);
}

void disableRawMode(void) {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &cfg.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode(void) {
    if (tcgetattr(STDIN_FILENO, &cfg.orig_termios) == -1)
        die("tcgetattr");

    atexit(disableRawMode);

    struct termios raw = cfg.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    return c;
}

int getCursorPosition(int *rows, int *cols) {
    char buf[16];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

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

int getWindowSize(int *rows, int *cols) {
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(&cfg.screenrows, &cfg.screencols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations **/
int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int i = 0; i < cx; i++) {
        if (row->chars[cx] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);

        rx++;
    }

    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t')
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0) row->render[idx++] = ' ';

        } else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    cfg.row = realloc(cfg.row, sizeof(erow) * (cfg.numrows + 1));

    int at = cfg.numrows;
    cfg.row[at].size = len;
    cfg.row[at].chars = malloc(len + 1);
    memcpy(cfg.row[at].chars, s, len);
    cfg.row[at].chars[len] = '\0';
    cfg.numrows++;

    cfg.row[at].rsize = 0;
    cfg.row[at].render = NULL;
    editorUpdateRow(&cfg.row[at]);
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
}

/*** editor operations ***/
void editorInsertChar(int c) {
    if (cfg.cy == cfg.numrows)
        editorAppendRow("", 0);

    editorRowInsertChar(&cfg.row[cfg.cy], cfg.cx, c);
    cfg.cx++;
}

/*** file i/o ***/
char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int i = 0; i < cfg.numrows; i++) {
        totlen += cfg.row[i].size + 1;
    }
    *buflen = totlen;

    char *buf = (char *) malloc(totlen);
    char *p = buf;
    for (int i = 0; i < cfg.numrows; i++) {
        memcpy(p, cfg.row[i].chars, cfg.row[i].size);
        p += cfg.row[i].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void editorOpen(char *filename) {
    free(cfg.filename);
    cfg.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while ((linelen > 0) && isspace(line[linelen - 1]))
        {
            linelen--;
        }

        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
}

void editorSave(void) {
    if (cfg.filename == NULL) return;

    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(cfg.filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        editorSetStatusMessage("Failed to save: %s", strerror(errno));
        free(buf);
        return;
    }

    if (ftruncate(fd, len) == -1) {
        editorSetStatusMessage("Failed to write: %s", strerror(errno));
        close(fd);
        free(buf);
        return;
    }

    if (write(fd, buf, len) != len) {
        editorSetStatusMessage("Failed to write: %s", strerror(errno));
        close(fd);
        free(buf);
        return;
    }

    editorSetStatusMessage("Wrote %d bytes to \"%s\"", len, cfg.filename);
}

/*** output ***/
// TODO implement SCROLLZONEing
void editorScroll(void) {
    cfg.rx = 0;
    if (cfg.cy < cfg.numrows) {
        cfg.rx = editorRowCxToRx(&cfg.row[cfg.cy], cfg.cx);
    }

    if (cfg.cy < cfg.rowoff) {
        cfg.rowoff = cfg.cy;
    }

    if (cfg.cy >= (cfg.rowoff + cfg.screenrows)) {
        cfg.rowoff = cfg.cy - cfg.screenrows + 1;
    }

    if (cfg.cx < cfg.coloff) {
        cfg.coloff = cfg.rx;
    }

    if (cfg.cx >= cfg.coloff + cfg.screencols) {
        cfg.coloff = cfg.rx - cfg.screencols + 1;
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    char *name = cfg.filename ? cfg.filename : "[No Name]";
    int len = snprintf(status, sizeof(status), "%.20s - %d lines", name, cfg.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", cfg.cy + 1, cfg.numrows);
    if (len > cfg.screencols)
        len = cfg.screencols;
    abAppend(ab, status, len);

    while (len < cfg.screencols) {
        if (cfg.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;

        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);

    switch (cfg.mode) {
        case NORMAL:
            abAppend(ab, "-- NORMAL -- ", 13);
            break;
        case INSERT:
            abAppend(ab, "-- INSERT -- ", 13);
            break;
        case COMMAND:
            abAppend(ab, ":", 1);
            return;
    }

    int msglen = strlen(cfg.statusmsg);
    if (msglen > cfg.screencols)
        msglen = cfg.screencols;

    if (msglen && time(NULL) - cfg.msg_time < MSG_SUSTAIN)
        abAppend(ab, cfg.statusmsg, msglen);
}

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < cfg.screenrows; y++) {
        int findex = y + cfg.rowoff;
        if (findex >= cfg.numrows) {
            if (cfg.numrows == 0 && y == cfg.screenrows / 3) {
                char msg[80];
                int msglen = snprintf(msg, sizeof(msg),
                        "Bon Jour -- version %s", JOUR_VERSION);

                if (msglen > cfg.screencols) msglen = cfg.screencols;

                int padding = (cfg.screencols - msglen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, msg, msglen);

            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = cfg.row[findex].rsize - cfg.coloff;
            if (len < 0) len = 0;
            if (len > cfg.screencols) len = cfg.screencols;
            abAppend(ab, &cfg.row[findex].render[cfg.coloff], len);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void editorRefreshScreen(int flag) {
    if (flag == R_SCROLL)
        editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
            (cfg.cy - cfg.rowoff) + 1,       // x
            (cfg.rx - cfg.coloff) + 1        // y
    );
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cfg.statusmsg, sizeof(cfg.statusmsg), fmt, ap);
    va_end(ap);
    cfg.msg_time = time(NULL);
}

/*** input ***/
void editorMoveCursor(int key) {
    erow *row = (cfg.cy >= cfg.numrows) ? NULL : &cfg.row[cfg.cy];

    switch (key) {
        case MOVE_LEFT:
            if (cfg.cx > 0)
                cfg.cx--;

        break;
        case MOVE_DOWN:
            if (cfg.cy < cfg.numrows)
                cfg.cy++;

        break;
        case MOVE_UP:
            if (cfg.cy > 0)
                cfg.cy--;

        break;
        case MOVE_RIGHT:
            if (row && cfg.cx < row->size - 1)
                cfg.cx++;

        break;
        case TOP:
            cfg.cy = 0;

        break;
        case BOTTOM:
            cfg.cy = cfg.numrows ? cfg.numrows - 1 : 0;

        break;

        case CMDLINE:
            cfg.lastx = cfg.cx;
            cfg.lasty = cfg.cy;
            cfg.cy = cfg.numrows + 1;
            cfg.rx = 1;
            return;

        case LINE_START:
            cfg.cx = 0;

        break;
        case LINE_END:
            cfg.cx = cfg.row[cfg.cy].rsize - 1;
        break;
    }

    row = (cfg.cy >= cfg.numrows) ? NULL : &cfg.row[cfg.cy];
    int rowlen = row ? (row->size) : 0;
    if (cfg.cx >= rowlen)
        cfg.cx = rowlen ? rowlen - 1 : 0;
}

void editorProcessCommand(void) {
    int c;
    int i = 0;
    char cmd[80];
    while ((c = editorReadKey()) != '\r' && i < 80) {
        cmd[i] = c;
        i++;
    }

    for (int j = 0; j < i; j++) {
        editorInsertChar(cmd[j]);

        switch (cmd[j]) {
            case 'w':
                editorSave();
            case 'q':
                write(STDOUT_FILENO, "\x1b[2J", 4);
                write(STDOUT_FILENO, "\x1b[H", 3);
                exit(0);
            break;
            default:
                if (!isdigit(cmd[0])) {
                    editorSetStatusMessage("Unknown command '%s'", &cmd[0]);
                    cfg.mode = NORMAL;
                    return;
                }

                cfg.cy = atoi(&cmd[0]);
            break;
        }
    }

    cfg.cx = cfg.lastx;
    cfg.cy = cfg.lasty;
    cfg.mode = NORMAL;
}

void editorProcessMotion(int c) {
    switch (c) {
        case 'i': // escape
            cfg.mode = INSERT;
        break;
        case ':':
        case ' ':
            cfg.mode = COMMAND;
            editorMoveCursor(CMDLINE);
/*
            editorRefreshScreen(R_NOSCROLL);
            editorProcessCommand();
*/
        break;
        case CTRL_PLUS('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case 'a':
            cfg.cx++;
            cfg.mode = INSERT;
            break;
        case 'A':
            cfg.cx = cfg.row[cfg.cy].size;
            cfg.mode = INSERT;
            break;
        case 'g':
            c = editorReadKey();
            if (c == 'g') {
                editorMoveCursor(TOP);
            }
            break;
        case 'G':
            editorMoveCursor(BOTTOM);
            break;
        case '0':
            editorMoveCursor(LINE_START);
            break;
        case '$':
            editorMoveCursor(LINE_END);
            break;
        case MOVE_LEFT:
        case MOVE_DOWN:
        case MOVE_UP:
        case MOVE_RIGHT:
            editorMoveCursor(c);
            break;
        case DELETE:
            break;
    }
}

void editorProcessKeypress(void) {
    int c = editorReadKey();

    if (cfg.mode == NORMAL) {
        editorProcessMotion(c);
    } else if (cfg.mode == COMMAND) {
        editorProcessCommand();
    } else {
        if (c == '\x1b') {
            cfg.mode = NORMAL;
            return;
        }

        editorInsertChar(c);
    }
}

/*** init ***/
void initEditor(void) {
    cfg.cx = 0;
    cfg.cy = 0;
    cfg.rx = 0;
    cfg.rowoff = 0;
    cfg.coloff = 0;
    cfg.numrows = 0;
    cfg.row = NULL;
    cfg.filename = NULL;
    cfg.statusmsg[0] = '\0';
    cfg.msg_time = 0;

    if (getWindowSize(&cfg.screenrows, &cfg.screencols) == -1)
        die("getWindowSize");

    cfg.cmdrow = cfg.screenrows;
    cfg.screenrows -= 2;
}

int main(int argc, char **argv) {
    enableRawMode();
    initEditor();

    if (argc >= 2)
        editorOpen(argv[1]);

    editorSetStatusMessage(":q to quit");

    while (1)
    {
        editorRefreshScreen(R_SCROLL);
        editorProcessKeypress();
    }
    return 0;
}
