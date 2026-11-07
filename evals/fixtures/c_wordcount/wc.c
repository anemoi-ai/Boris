/* wc.c - print the line, word and character counts of standard input. */
#include <ctype.h>
#include <stdio.h>

int main(void)
{
	long lines = 0, words = 0, chars = 0;
	int c, in_word = 0;

	while ((c = getchar()) != EOF) {
		chars++;
		if (c == '\n')
			lines++;
		if (isspace(c)) {
			if (in_word)
				words++;
			in_word = 0;
		} else {
			in_word = 1;
		}
	}
	printf("%ld %ld %ld\n", lines, words, chars);
	return 0;
}
