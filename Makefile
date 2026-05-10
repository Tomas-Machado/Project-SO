CC = gcc
# O CFLAGS usa --cflags para encontrar os cabeçalhos (.h)
CFLAGS = -Wall -g -Iinclude `pkg-config --cflags glib-2.0`

# O LDFLAGS usa --libs para encontrar o código binário da biblioteca
LDFLAGS = `pkg-config --libs glib-2.0`

all: folders controller runner

controller: bin/controller

runner: bin/runner

folders:
	@mkdir -p src include obj bin tmp

bin/controller: obj/controller.o
	$(CC) $^ -o $@ $(LDFLAGS)

bin/runner: obj/runner.o
	$(CC) $^ -o $@ $(LDFLAGS)

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf obj/ tmp/ bin/