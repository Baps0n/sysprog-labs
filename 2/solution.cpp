#include "parser.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static void
execute_command_line(const struct command_line *line)
{
	assert(line != NULL);
	if (line->out_type == OUTPUT_TYPE_STDOUT) {
	} else if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
	} else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
	} else {
		assert(false);
	}
	
	for (const expr &e : line->exprs) {
		if (e.type == EXPR_TYPE_COMMAND) {
			if (e.cmd->exe == "cd")	{
				if (chdir(e.cmd->args[0].data()) != 0) {
					perror("chdir");
					exit(1);
				}
				continue;
			}
			else if (e.cmd->exe == "exit") {
				exit(0);
			}
			
			std::vector<char*> argv;
			argv.push_back(const_cast<char*>(e.cmd->exe.data()));
			for (const std::string& arg : e.cmd->args) {
				argv.push_back(const_cast<char*>(arg.data()));
			}
			argv.push_back(nullptr);
			pid_t pid = fork();

			if (pid < 0) {
				perror("fork");
				return;
			}
			else if (pid == 0) {
				execvp(e.cmd->exe.data(), argv.data());
				perror("execvp");
				exit(1);
			}
			
			if (!line->is_background) {
				int status;
				waitpid(pid, &status, 0);
			}
		} else if (e.type == EXPR_TYPE_PIPE) {
			printf("\tPIPE\n");
		} else if (e.type == EXPR_TYPE_AND) {
			printf("\tAND\n");
		} else if (e.type == EXPR_TYPE_OR) {
			printf("\tOR\n");
		} else {
			assert(false);
		}
	}
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			execute_command_line(line);
			delete line;
		}
	}
	parser_delete(p);
	return 0;
}
