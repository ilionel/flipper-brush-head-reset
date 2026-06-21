# Host-side checks for the brush-head reset project.
# The Flipper app itself is built with ufbt (see README).

.PHONY: test test-c test-py icon clean

test: test-c test-py ## run all host checks

test-c: ## compile & run the C password unit test
	cc -Wall -Wextra -I. soniclear_pwd.c test/test_pwd.c -o test/test_pwd
	./test/test_pwd

test-py: ## run the Python host-tool self-test
	python3 soniclear.py --selftest

icon: ## regenerate the app icon (needs Pillow)
	python3 gen_icon.py

clean:
	rm -f test/test_pwd
