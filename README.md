# tinyniSh
`tinyniSh` is a simple shell program written in C. It supports:



## Compiling

You can use `gcc` to compile the source:

`gcc --std=gnu99 -o tinynish tinynish.c`

Or, if you want to get fancy, you can use the `CMake` build generation system:

`cmake -S . -B ./build/`

This will create a `build/` subdirectory which will, if you're on a Unix-based system, have a normal `Makefile` 



## Usage

Some features:

- Input beginning with `#` is treated as comments and ignored
- Any instance of `$$` is expanded into the process ID of the shell itself (there is no other variable expansion)
- `exit` to leave
- `status` prints out either the exit status or the terminating signal of the last foreground process run by the shell
- Commands with `&` at the end are run in the background (all others are run in the foreground). Their process ID will be printed when the command is run, and if it has finished by the next input from the user, its exit status will be printed then.
- There is no piping.
- `CTRL+C` will terminate any foreground process. 
- `Ctrl+Z` will cause the shell to enter a mode where background commands are disabled: `&` at the end is ignored and the command is run in the foreground. `Ctrl+Z` again will toggle out of this state.



## Example

Here's a gif of the shell running some commands:

![demo](demo.gif)

A transcript of the commands run -

```
: ls
CMakeLists.txt	LICENSE		README.md	tinynish	tinynish.c
: touch junk
: ls > junk
: cat junk
CMakeLists.txt
LICENSE
README.md
junk
tinynish
tinynish.c
: wc < junk
       6       6      58
: status
exit value 0
: test -f thisfiledoesntexist
: status
exit value 1
: sleep 5
^Cterminated by signal 2
: sleep 15 &
background pid is 10410
: ps
  PID TTY           TIME CMD
 2921 ttys000    0:00.33 -zsh
 3475 ttys001    0:00.66 -zsh
 9858 ttys001    0:00.01 ./tinynish
 9899 ttys002    0:00.17 -zsh
10372 ttys002    0:00.01 ./tinynish
10410 ttys002    0:00.00 sleep 15
: # this is a comment line
:
background pid 10410 is done: exit value 0
: sleep 30 &
background pid is 10413
: kill -15 10413
background pid 10413 is done: terminated by signal 15
: echo $$
10372
: ^ZEntering foreground-only mode (& is now ignored)
: sleep 5 &
: ^ZExiting foreground-only mode
: exit
```





















