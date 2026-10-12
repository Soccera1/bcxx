CC = tcc
CFLAGS = -std=c99 -Wall -Wextra
CPPFLAGS = -Isrc
SOURCES = src/driver.c src/util.c src/diagnostic.c src/lexer.c src/pp_expr.c src/preprocessor.c
OBJECTS = $(SOURCES:src/%.c=build/%.o)

.PHONY: all check check-self clean check-gcc check-clang check-sanitize
all: build/bcxx

build/bcxx: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJECTS)

build/%.o: src/%.c src/bcxx.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

check: build/bcxx
	python3 tests/test_driver.py $(CURDIR)/build/bcxx

check-self: build/bcxx
	python3 tests/check_self.py $(CURDIR)/build/bcxx

check-gcc:
	$(MAKE) clean
	$(MAKE) check CC=gcc CFLAGS='-std=c99 -Wall -Wextra -Wpedantic -Werror -g'

check-clang:
	$(MAKE) clean
	$(MAKE) check CC=clang CFLAGS='-std=c99 -Wall -Wextra -Wpedantic -Werror -g'

check-sanitize:
	$(MAKE) clean
	$(MAKE) check CC=clang CFLAGS='-std=c99 -Wall -Wextra -Wpedantic -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer'

clean:
	rm -rf build
