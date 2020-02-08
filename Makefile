all:
	gcc sched.c -g -pthread -D_XOPEN_SOURCE

clean:
	rm -f a.out

stop:
	pkill a.out
