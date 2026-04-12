all:
	gcc -Wall -o backend/gc_server backend/gc_server.c

run: all
	./backend/gc_server

clean:
	rm -f backend/gc_server
