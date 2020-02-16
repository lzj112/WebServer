web:http_conn.o main.o
	g++ http_conn.o main.o -o web -lpthread
http_conn.o:http_conn.cpp http_conn.h
	g++ -c http_conn.cpp -o http_conn.o -lpthread
main.o:main.cpp threadpool.h locker.h
	g++ -c main.cpp -o main.o -lpthread
clean:
	rm -rf *.o web
