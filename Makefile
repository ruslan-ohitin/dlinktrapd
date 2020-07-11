

all:
	gcc -Wall -O1 -I /usr/include -L /usr/lib/x86_64-linux-gnu/  -o dlinktrapd dlinktrapd.c -lpcre

