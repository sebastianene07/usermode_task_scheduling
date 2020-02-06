all:
	gcc sched.c -g -m32 -pthread

clean:
	rm -f a.out

stop:
	pkill a.out
