/*
* main.c
*
* Responsible for the main control loop of the conch shell
*
* Author: Kyle Sherman
* Created: 2025-05-22
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <ctype.h>
#include <errno.h>

struct termios orig_termios;

void die(const char *string) {
  perror(string);
  exit(1);
}

// Restore terminal to original attributes upon program exit
// store termios struct in its original state and set attribute to apply
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1) {
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

  if(tcgetattr(STDIN_FILENO, &orig_termios) == -1) {
    die("tcgetattr");
  }
  atexit(disableRawMode); // execute at program exit

  struct termios raw = orig_termios;
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

int main() {
  enableRawMode();

  while(1) {
    char input = '\0';
    if(read(STDIN_FILENO, &input, 1) == -1 && errno != EAGAIN) {
      die("read");
    }

    if(iscntrl(input)) {
      printf("%d\r\n", input);
    } else {
      printf("%d ('%c')\r\n", input, input);
    }

    if(input == 'q') {
      break;
    }
  }

  return 0;
}
