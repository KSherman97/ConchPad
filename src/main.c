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

// Takes the control key and bitwise-ANDS the character value with 00011111
// this basically mimics what the terminal already does by stripping bits 5 & 6
// from the key combination
#define CTRL_KEY(k) ((k) & 0x1f) // define what the CTRL_KEY bytecode is


/** data **/

struct editorConfig {
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
char editorReadKey() {
  int nread;
  char input;

  while((nread = read(STDIN_FILENO, &input, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  return input;
}

// uses system function and struct winsize from sys/ioctl.h
// returns -1 on fail
// on success returns a struct containing number of rows and columns
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if(1 || ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }
    editorReadKey();
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/** output **/

// draw each row of the buffer text being edited
// for now draws a tilde in each row meaning the row is not part of the file
// and cannot contain any text
// we don't know the terminal size yet, so default to 24 rows
void editorDrawRows() {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

// write 4 bytes to the terminal
// \x1b is the escape character
// escape char is always followed by '['
// J is the erase in display command
// escape sequence commands take args that come before.
// 1 would clear screen up to cursor; 2 clears entire display
void editorScreenRefresh() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3); // reposition the cursor to top left

  editorDrawRows();
  write(STDOUT_FILENO, "\x1b[H", 3);
}

/** input **/

// define the controls for our editor
void editorProcessKeypress() {
  char input = editorReadKey();

  switch(input) {
    case CTRL_KEY('q'):
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3); 
      exit(0);
      break; 
  }
}


/** init **/

// initialize all fields in the E struct [editorConfig]
void initEditor() {
  if(getWindowSize(&E.screenrows, &E.screencols) == -1) {
    die("getWindowSize");
  }
}

int main() {
  enableRawMode();
  initEditor();

  while(1) {
    editorScreenRefresh();
    editorProcessKeypress();
  }

  return 0;
}
