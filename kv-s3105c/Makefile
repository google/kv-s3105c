kvscanner: kvscanner.c kvs3105usb.c monitor.c
	gcc -g -o $@ -I. $^ -O2 -Wall -std=c99 -lusb-1.0

clean:
	rm -f *.o kvscanner
