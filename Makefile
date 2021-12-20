# extra includes paths (-I options)
INCLUDE = -I/usr/X11R6/include 

# extra libraries paths (-L options)
LIB = -L/usr/X11R6/lib

# extra libraries (-l options)
LDADD = -lX11 -lsndio 

# compiler flags
CFLAGS=-g -O2 -Wall

# binaries and man pages will be installed here
BIN_DIR = /usr/local/bin
MAN1_DIR = /usr/local/man/man1

PROG = sndiokeys
OBJS = sndiokeys.o

sndiokeys:	${OBJS}
		${CC} -o ${PROG} ${OBJS} ${LIB} ${LDADD}

sndiokeys.o:	sndiokeys.c
		${CC} ${CFLAGS} ${INCLUDE} -c sndiokeys.c

install:
		mkdir -p ${DESTDIR}${BIN_DIR} ${DESTDIR}${MAN1_DIR}
		cp ${PROG} ${DESTDIR}${BIN_DIR}
		cp ${PROG:=.1} ${DESTDIR}${MAN1_DIR}

uninstall:
		cd ${DESTDIR}${BIN_DIR} && rm -f ${PROG}
		cd ${DESTDIR}${MAN1_DIR} && rm -f ${PROG:=.1}

clean:
		rm -f -- ${PROG} ${OBJS}
