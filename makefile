# hello
### https://www.cs.swarthmore.edu/~newhall/unixhelp/howto_makefiles.html ###

CC := gcc
CFLAGS := -Wall -Wextra -g
MAIN := uvim

all: $(MAIN)
	@echo \`$(MAIN)\' hath compiled

$(MAIN): main.c
	$(CC) $(CFLAGS) main.c -o $(MAIN)

clean:
	$(RM) jour

