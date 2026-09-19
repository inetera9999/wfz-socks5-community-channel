cc = gcc
flag = -Wall -g -std=c99
target = socks5
targetclt = clientdemo
object = socks5.o buff.o Utils.o 
objectclt = buff.o Utils.o clientdemo.o
src = socks5.c buff.c

all : $(object) clientdemo.o
	$(cc) $(flag) -O0 -o $(target) $(object)
	$(cc) $(flag) -O0 -o $(targetclt) $(objectclt)

.SUFFIXES: .c .o
.PHONY:	clean
# socks5.o : socks5.c
# 	$(cc) $(flag) -c -o socks5.o socks5.c

# buff.o : buff.c
# 	$(cc) $(flag) -c -o buff.o buff.c

.c.o:
	$(cc) $(flag) -c -g  -O0 -o $@ $<


clean:
	rm *.o $(target) core.* vgcore.*
