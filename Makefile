

all:
	cc -Wall -O1 -I/usr/local/include -L/usr/local/lib -lpcre -o dlinktrapd dlinktrapd.c


install:
	install dlinktrapd /usr/local/sbin/
	install dlinktrapd.sh /usr/local/etc/rc.d/dlinktrapd
	chmod 555 /usr/local/etc/rc.d/dlinktrapd

clean:
	rm -f *.o dlinktrapd

deinstall:
.if exists(/var/run/dlinktrapd.pid)
	/usr/local/etc/rc.d/dlinktrapd stop
.endif
	rm -f /usr/local/sbin/dlinktrapd
	rm -f /usr/local/etc/rc.d/dlinktrapd
