/*
* main.c
*
* Responsible for the main control loop of ConchPad
*
* Author: Kyle Sherman
* Created: 2025-08-05
*/

/** includes **/

// even though these are defines, some of our includes rely on them to determine what features to expose
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

/** defines **/

#define ConchPad_VERSION "0.0.1"
#define ConchPad_TAB_STOP 8
#define ConchPad_QUIT_TIMES 2

// Takes the control key and bitwise-ANDS the character value with 00011111
// this basically mimics what the terminal already does by stripping bits 5 & 6
// from the key combination
#define CTRL_KEY(k) ((k) & 0x1f) // define what the CTRL_KEY bytecode is

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT = 2000,
  ARROW_UP = 3000,
  ARROW_DOWN = 4000,
  PAGE_UP,
  PAGE_DOWN,
  HOME_KEY,
  END_KEY,
  DEL_KEY
};

/** data **/

// struct containing info about a row in the editor
typedef struct erow {
  int size; // length of a row in the filestream
  int rsize; // render size
  char *chars; // content of a row in the filestream
  char *render; // render contents
} erow;

struct editorConfig {
  int cx; // cursor x pos
  int cy; // cursor y pos
  int rx; // horizontal coordinate (required for handling tabs)
  int rowoff; // row offset
  int coloff; // column offset
  int screenrows; // column size of the screen
  int screencols; // row size of the screen
  int numrows; // number of rows in the filestream
  erow *row; // contents of the rows in the filestream
  int dirty; // a file is dirty if unsaved changes have occurred
  char *filename; // save a copy of the openned file's name
  char statusmsg[80]; // storing the status message string
  time_t statusmsg_time; // storing the status message time
  struct termios orig_termios;
};

struct editorConfig E;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);
void editorScreenRefresh();
char *editorPrompt(char *prompt);

/** terminal **/

void die(const char *string) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3); 

  perror(string);
  exit(1);
}

// Restore terminal to original attributes upon program exit
// store termios struct in its original state and set attribute to apply
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

// For a text editor, we need to capture raw input. Default terminal behavior
// is to wait for an enter key; this is called canonical mode (cooked mode)
// Disable the following flags in raw mode:
//  1. ECHO: Echos input to the terminal
//  2. ICONON: Read input byte-by-byte instead of line-by-line
//  3. ISIG: Disable the default ctrl-c and ctrl-z signals
//  4. IXON: Disables CTRL-S & CTRL-Q used for software flow control
//  5. IEXTEN: Disable CTRL-V - causes system to wait for another character input
//  6. ICRNL: Disable CTRL-M - interprets carriage returns as newline characters
//  7. OPOST: disable all output processing features (i.e. \n -> \r\n)
void enableRawMode(){

  if(tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }
  atexit(disableRawMode); // execute at program exit

  struct termios raw = E.orig_termios;
  raw.c_lflag &= ~(ECHO); // disable the ECHO flag - printing keystrokes back
  raw.c_lflag &= ~(ICANON); // disable canonical flag
  raw.c_lflag &= ~(ISIG); // disable (SIGINT & SIGSTP) signals (causes suspend)
  raw.c_iflag &= ~(IXON); // disable (CTRL-S) and (CTRL-Q)
  raw.c_lflag &= ~(IEXTEN); // disable (ctrl-v) and ctrl-o (macOS)
  raw.c_iflag &= ~(ICRNL); // disable ctrl-m (forces ctrl-m to read as 13 [carriage return] instead of 10 [newline])
  raw.c_oflag &= ~(OPOST); // disable all output processing features

  // these flags are mostly inconsiquential as they are typically turned off by default
  // tradition holds, that these are a part of 'raw mode' so I want to keep with the standards
  // even though there should be no noticible effects
  raw.c_cflag |= (CS8);
  raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);

  // configure a few terminal settings
  // VMIN: number of bytes input needed before read() can return
  // VTIME: max amount of time to wait before read() returns [in 100 ms intervals]
  raw.c_cc[VMIN] = 0; // return as soon as any input can be read
  raw.c_cc[VTIME] = 1; // min value - since we are reading every keystroke, 1 is fine (bash on windows ignores this)


  if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

// wait for one keypress and return it
// TODO: Escape sequences - reading multiple bytes that that represent a single
//        keypress like arrow keys
int editorReadKey() {
  int nread;
  char input;

  while((nread = read(STDIN_FILENO, &input, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  if (input == '\x1b') {
    char seq[3];

    if(read(STDIN_FILENO, &seq[0], 1) != 1) {
      return '\x1b';
    }

    if(read(STDIN_FILENO, &seq[1], 1) != 1) {
      return '\x1b';
    }

    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if(read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }

        if(seq[2] == '~') {
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
        switch (seq[1]) {
          case 'A': return ARROW_UP;
          case 'B': return ARROW_DOWN;
          case 'C': return ARROW_RIGHT;
          case 'D': return ARROW_LEFT;
          case 'H': return HOME_KEY;
          case 'F': return END_KEY;
      }
    }
  } else if (seq[0] == '0') {
    switch (seq[1]) {
      case 'H': return HOME_KEY;
      case 'F': return END_KEY;
    }
  }
    return '\x1b';
  } else {
    return input;
  }
}

// use device status report to query the terminal for status information
// providing an argument of 6 to the n command we can read from the stdin
// source: https://vt100.net/docs/vt100-ug/chapter3.html#CPR
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while(i < sizeof(buf) - 1) {
    if(read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }
    if(buf[i] == 'R') {
      break;
    }

    i++;
  }

  if (buf[0] != '\x1b' || buf[1] != '[') return -1;
  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

  return 0;
}

// uses system function and struct winsize from sys/ioctl.h
// returns -1 on fail
// on success returns a struct containing number of rows and columns
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/** row operations **/

int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for(j = 0; j < cx; j++) {
    if(row->chars[j] == '\t') {
      rx += (ConchPad_TAB_STOP - 1) - (rx % ConchPad_TAB_STOP);
    }

    rx++;
  }

  return rx;
}

void editorUpdateRow(erow *row) {
  int tabs = 0;
  int j;
  for(j = 0; j < row->size; j++) {
    if(row->chars[j] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (ConchPad_TAB_STOP - 1) + 1);


  int idx = 0;
  for(j = 0; j < row->size; j++) {
    if(row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % ConchPad_TAB_STOP != 0) {
        row->render[idx++] = ' ';
      }
    } else {
      row->render[idx++] = row->chars[j];
    }
  }

  row->render[idx] = '\0';
  row->rsize = idx;
}

void editorInsertRow(int at, char *string, size_t len) {
  if(at < 0 || at > E.numrows) {
    return;
  }

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, string, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

void editorDelRow(int at) {
  if(at < 0 || at >= E.numrows) {
    return;
  }

  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
  if(at < 0 || at > row->size) {
    at = row->size;
  }

  row->chars = realloc(row->chars, row->size + 2);

  // I used memmove because it is safe when the src and dest overlap
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowAppendString(erow *row, char *string, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], string, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
  if(at < 0 || at >= row->size) {
    return;
  }

  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

void editorDelChar() {
  if (E.cy == E.numrows) {
    return;
  }
  if(E.cx == 0 && E.cy == 0) {
    return;
  }

  erow *row = &E.row[E.cy];
  if(E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelChar(E.cy);
    E.cy--;
  }
}

/** editor operations **/
void editorInsertChar(int c) {
  if(E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }

  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewLine() {
  if(E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->size = E.cx;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }

  E.cy++;
  E.cx = 0;
}

/** file i/o **/

char *editorRowsToString(int *buflen) {
  int totlen = 0;
  int j;

  for(j = 0; j < E.numrows; j++) {
    totlen += E.row[j].size + 1;
  }

  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;

  for(j = 0; j < E.numrows; j++) {
    memcpy(p, E.row[j].chars, E.row[j].size);
    p += E.row[j].size;
    *p = '\n';
    p++;
  }

  return buf;
}

void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");

  if(!fp) {
    die("fopen");
  }

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;

  while((linelen = getline(&line, &linecap, fp)) != -1){
    while(linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) {
      linelen--;
    }
    editorInsertRow(E.numrows, line, linelen);
  }

  free(line);
  fclose(fp);
  E.dirty = 0;
}

void editorSave() {
  if(E.filename == NULL) {
    E.filename = editorPrompt("save as: %s (esc to cancel)");
    if(E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if(fd != -1) {
    if(ftruncate(fd, len) != -1) {
      if(write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O Error: %s", strerror(errno));
}

/** append buffer **/

// create an append buffer struct to replace direct STDOUT writes
struct abuf {
  char *b;
  int len;
};

#define ABUF_INIT {NULL, 0}

// Append to the buffer via realloc and memcpy to chunk our write calls
void abAppend(struct abuf *ab, const char *s, int len) {
  char *new = realloc(ab -> b, ab -> len + len);

  if(new == NULL) {
    return;
  }

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

// free the append buffer from memory
void abFree(struct abuf *ab) {
  free(ab->b);
}

/** output **/

void editorScroll() {
  E.rx = E.cx;

  if(E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if(E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  if(E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }
  if(E.rx < E.coloff) {
    E.coloff = E.rx;
  }
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

// draw each row of the buffer text being edited
// for now draws a tilde in each row meaning the row is not part of the file
// and cannot contain any text
// we don't know the terminal size yet, so default to 24 rows
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if(filerow >= E.numrows) {
      // Add in a welcome message to the top of the screen
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
          "ConchPad editor -- version %s", ConchPad_VERSION);
        if(welcomelen > E.screencols) {
          welcomelen = E.screencols;
        }

        int padding = (E.screencols - welcomelen) / 2;
        if(padding) {
          abAppend(ab, "~", 1);
          padding--;
        }
        while(padding--) {
          abAppend(ab, " ", 1);
        }

        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      
      if(len < 0) {
        len = 0;
      }

      if(len > E.screencols) {
        len = E.screencols;
      }

      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    // redraw each line as it is edited (replace previous whole screen refresh)
    abAppend(ab, "\x1b[K", 3); // source: http://vt100.net/docs/vt100-ug/chapter3.html#EL
    abAppend(ab, "\r\n", 2);
  }
}

void editorDrawStatusBar(struct abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);
  
  char status[80];
  char rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
    E.filename ? E.filename : "[No Name]", E.numrows,
    E.dirty ? "(modified)" : "");

  int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
    E.cy + 1, E.numrows);

  if(len > E.screencols) {
    len = E.screencols;
  }

  abAppend(ab, status, len);

  while(len < E.screencols) {
    if(E.screencols - len == rlen) {
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

void editorDrawMessageBar(struct abuf *ab){ 
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if(msglen > E.screencols) {
    msglen = E.screencols;
  }
  if(msglen && time(NULL) - E.statusmsg_time < 5) {
    abAppend(ab, E.statusmsg, msglen);
  }
}

// write 4 bytes to the terminal
// \x1b is the escape character
// escape char is always followed by '['
// J is the erase in display command
// escape sequence commands take args that come before.
// 1 would clear screen up to cursor; 2 clears entire display
// Source for cursor commands: https://vt100.net/docs/vt100-ug/chapter3.html#S3.3.4
void editorScreenRefresh() {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide the cursor from view (stops flickering)
  // abAppend(&ab, "\x1b[2J", 4); // reset entire screen
  abAppend(&ab, "\x1b[H", 3); // reposition the cursor to top left

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];
  // set cursor argument [H] the the x, y coordinates
  // then write to the buffer
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1, (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // set the cursor to be visible again

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/** input **/

char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while(1) {
    editorSetStatusMessage(prompt, buf);
    editorScreenRefresh();

    int c = editorReadKey();
    if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {

    } else if(c == '\x1b') {
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if(c == '\r') {
      if(buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if(!iscntrl(c) && c < 128) {
      if(buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

void editorCursorMove(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
    case ARROW_LEFT:
      if(E.cx != 0) {
        E.cx--;
      } else if (E.cy > 0) {
        E.cy--;
        E.cx = E.row[E.cy].size;
      }
      break;
    case ARROW_RIGHT:
      if(row && E.cx < row->size) {
        E.cx++;
      } else if(row && E.cx == row->size) {
        E.cy++;
        E.cx = 0;
      }
      break;
    case ARROW_UP:
      if(E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if(E.cy != E.numrows) {
        E.cy++;
      }
      break;
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if(E.cx > rowlen) {
    E.cx = rowlen;
  }
}

// define the controls for our editor
void editorProcessKeypress() {
  static int quite_times = ConchPad_QUIT_TIMES;

  int input = editorReadKey();

  switch(input) { 
    case '\r':
      editorInsertNewLine();
      break;

    case CTRL_KEY('q'):
      if(E.dirty && quite_times > 0) {
        editorSetStatusMessage("Warning! File has unsaved changes. "
          "Press Ctrl-Q %d more times to quit.", quite_times);
      }
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3); 
      exit(0);
      break; 

    case CTRL_KEY('s'):
      editorSave();
      break;

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      if(E.cy < E.numrows) {
        E.cx = E.screencols - 1;  
      }
      break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
      if (input == DEL_KEY) {
        editorCursorMove(ARROW_RIGHT);
      }
      editorDelChar();
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
        if(input == PAGE_UP) {
          E.cy = E.rowoff;
        } else if(input == PAGE_DOWN) {
          E.cy = E.rowoff + E.screenrows - 1;

          if(E.cy > E.numrows) {
            E.cy = E.numrows;
          }
        }

        int times = E.screenrows;
        while (times --) {
          editorCursorMove(input == PAGE_UP ? ARROW_UP: ARROW_DOWN);
        }
      }

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_RIGHT:
    case ARROW_LEFT:
      editorCursorMove(input);
      break;

    case CTRL_KEY('l'):
    case '\x1b':
      break;

    default:
      editorInsertChar(input);
      break;
  }

  quite_times = ConchPad_QUIT_TIMES;
}


/** init **/

// initialize all fields in the E struct [editorConfig]
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if(getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }

  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  write(STDOUT_FILENO, "\x1b[2J", 4);

  enableRawMode();
  initEditor();
  if(argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while(1) {
    editorScreenRefresh();
    editorProcessKeypress();
  }

  return 0;
}
