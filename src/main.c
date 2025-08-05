/*
* main.c
*
* Responsible for the main control loop of ConchPad
*
* Author: Kyle Sherman
* Created: 2025-08-05
*/

/** includes **/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>

/** defines **/

#define ConchPad_VERSION "0.0.1"

// Takes the control key and bitwise-ANDS the character value with 00011111
// this basically mimics what the terminal already does by stripping bits 5 & 6
// from the key combination
#define CTRL_KEY(k) ((k) & 0x1f) // define what the CTRL_KEY bytecode is

enum editorKey {
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

struct editorConfig {
  int cx; // cursor x pos
  int cy; // cursor y pos
  int screenrows;
  int screencols;
  struct termios orig_termios;
};

struct editorConfig E;

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

// draw each row of the buffer text being edited
// for now draws a tilde in each row meaning the row is not part of the file
// and cannot contain any text
// we don't know the terminal size yet, so default to 24 rows
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {

    // Add in a welcome message to the top of the screen
    if (y == E.screenrows / 3) {
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

    // redraw each line as it is edited (replace previous whole screen refresh)
    abAppend(ab, "\x1b[k", 3); // source: http://vt100.net/docs/vt100-ug/chapter3.html#EL
    if(y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
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
  struct abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l", 6); // hide the cursor from view (stops flickering)
  // abAppend(&ab, "\x1b[2J", 4); // reset entire screen
  abAppend(&ab, "\x1b[H", 3); // reposition the cursor to top left

  editorDrawRows(&ab);

  char buf[32];
  // set cursor argument [H] the the x, y coordinates
  // then write to the buffer
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6); // set the cursor to be visible again

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/** input **/

void editorCursorMove(int key) {
  switch (key) {
    case ARROW_LEFT:
      if(E.cx != 0) {
        E.cx--;
      }
      break;
    case ARROW_RIGHT:
      if(E.cx != E.screencols - 1) {
        E.cx++;
      }
      break;
    case ARROW_UP:
      if(E.cy != 0) {
        E.cy--;
      }
      break;
    case ARROW_DOWN:
      if(E.cy != E.screencols - 1) {
        E.cy++;
      }
      break;
  }
}

// define the controls for our editor
void editorProcessKeypress() {
  int input = editorReadKey();

  switch(input) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3); 
      exit(0);
      break; 

    case HOME_KEY:
      E.cx = 0;
      break;
    case END_KEY:
      E.cx = E.screencols - 1;
      break;

    case PAGE_UP:
    case PAGE_DOWN:
      {
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
  }
}


/** init **/

// initialize all fields in the E struct [editorConfig]
void initEditor() {
  E.cx = 0;
  E.cy = 0;

  if(getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
}

int main() {
  write(STDOUT_FILENO, "\x1b[2J", 4);

  enableRawMode();
  initEditor();

  while(1) {
    editorScreenRefresh();
    editorProcessKeypress();
  }

  return 0;
}
