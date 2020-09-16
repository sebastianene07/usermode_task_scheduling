OUT=test

all:
	gcc sched.c -g -pthread -D_XOPEN_SOURCE -o $(OUT)

clean:
	rm -f $(OUT)

stop:
	pkill $(OUT)
